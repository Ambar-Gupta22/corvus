#include "corvus/http_transport.h"

#ifdef CORVUS_HAS_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace corvus {
namespace {

// Non-2xx streamed replies keep a capped copy of the error payload in body so
// the caller can parse the provider's error JSON. Full (non-streamed) bodies
// are capped too so a hostile/huge response can't OOM the process.
constexpr std::size_t kErrorBodyCap = 256 * 1024;
constexpr std::size_t kMaxBodyBytes = 64 * 1024 * 1024;

struct SplitUrl {
    std::string base;  // "https://host[:port]"
    std::string path;  // "/v1/messages" ("/" if absent)
    bool https = false;
    bool ok = false;
};

bool hasCtl(const std::string& s) {
    return s.find('\r') != std::string::npos || s.find('\n') != std::string::npos;
}

SplitUrl splitUrl(const std::string& url) {
    SplitUrl out;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        out.https = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        rest = url.substr(7);
    } else {
        return out;
    }
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (hostport.empty()) return out;
    // Reject userinfo (user@host — would connect somewhere other than the
    // apparent host) and any whitespace/control char in the authority.
    if (hostport.find('@') != std::string::npos) return out;
    for (const unsigned char c : hostport) {
        if (c <= ' ') return out;
    }
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    out.base = (out.https ? "https://" : "http://") + hostport;
    out.ok = true;
    return out;
}

bool iequals(const std::string& a, const char* b) {
    const std::size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

class HttplibTransport final : public HttpTransport {
public:
    HttpResponse post(const HttpRequest& req, const ChunkCallback& onChunk,
                      const CancelToken& cancel) override {
        HttpResponse out;

        const SplitUrl u = splitUrl(req.url);
        if (!u.ok) {
            out.error = "invalid url: " + req.url;
            return out;
        }
#ifndef CORVUS_HAS_TLS
        if (u.https) {
            out.error =
                "https requested but corvus was built without TLS "
                "(OpenSSL not found at build time)";
            return out;
        }
#endif
        if (cancel.cancelled()) {
            out.error = "cancelled";
            return out;
        }

        httplib::Client cli(u.base);
        cli.set_connection_timeout(req.connectTimeout);
        cli.set_read_timeout(req.readTimeout);
        cli.set_write_timeout(req.readTimeout);
        cli.set_follow_location(false);

        httplib::Headers headers;
        std::string contentType = "application/json";
        for (const auto& h : req.headers) {
            // Reject CR/LF in either name or value: httplib validates CRLF only
            // in Request::set_header, which the multimap path below bypasses,
            // so an unchecked value could inject headers or smuggle a request.
            if (hasCtl(h.first) || hasCtl(h.second)) {
                out.error = "invalid header (contains CR/LF): " + h.first;
                return out;
            }
            if (iequals(h.first, "content-type")) {
                contentType = h.second;
            } else {
                headers.emplace(h.first, h.second);
            }
        }

        int status = 0;
        bool cancelled = false;
        bool aborted = false;
        bool overflow = false;
        std::string bodyBuf;  // full body (no onChunk) or non-2xx error tee

        httplib::Request hreq;
        hreq.method = "POST";
        hreq.path = u.path;
        hreq.headers = headers;
        hreq.body = req.body;
        hreq.set_header("Content-Type", contentType);
        // Fires before any body byte — gives us the status so we can route 2xx
        // stream bytes to onChunk but keep a non-2xx error payload in body.
        hreq.response_handler = [&](const httplib::Response& r) -> bool {
            status = r.status;
            return true;
        };
        hreq.content_receiver = [&](const char* data, std::size_t len, std::uint64_t,
                                    std::uint64_t) -> bool {
            if (cancel.cancelled()) {
                cancelled = true;
                return false;  // closes the socket — cancel is honored mid-transfer
            }
            const bool ok2xx = status / 100 == 2;
            if (onChunk && ok2xx) {
                if (!onChunk(data, len)) {
                    aborted = true;
                    return false;
                }
                return true;
            }
            // Buffer: whole body when non-streamed, or the capped error payload
            // on a non-2xx streamed reply.
            const std::size_t cap = onChunk ? kErrorBodyCap : kMaxBodyBytes;
            if (bodyBuf.size() < cap) {
                bodyBuf.append(data, std::min(len, cap - bodyBuf.size()));
            }
            if (bodyBuf.size() >= cap && !onChunk) {
                overflow = true;  // full-body cap hit: stop reading, fail cleanly
                return false;
            }
            return true;
        };

        const httplib::Result res = cli.send(hreq);

        if (!res) {
            if (cancelled || cancel.cancelled()) {
                out.error = "cancelled";
            } else if (aborted) {
                out.error = "aborted by receiver";
            } else if (overflow) {
                out.error = "response body exceeded cap";
            } else {
                out.error = httplib::to_string(res.error());
            }
            return out;  // status stays 0
        }

        out.status = res->status;
        out.headers.reserve(res->headers.size());
        for (const auto& h : res->headers) out.headers.emplace_back(h.first, h.second);
        out.body = std::move(bodyBuf);  // empty for a streamed 2xx (bytes went to onChunk)
        return out;
    }
};

}  // namespace

HttpTransportPtr defaultHttpTransport() { return std::make_shared<HttplibTransport>(); }

}  // namespace corvus
