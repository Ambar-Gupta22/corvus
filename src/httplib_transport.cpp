#include "corvus/http_transport.h"

#ifdef CORVUS_HAS_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
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

bool isCtl(unsigned char c) { return c < 0x20 || c == 0x7f; }

// RFC 9110 token: what a header name may contain. Rules out CR/LF, NUL,
// spaces and ':' (which would split the name into a forged value).
bool isHeaderName(const std::string& s) {
    if (s.empty()) return false;
    for (const unsigned char c : s) {
        if (std::isalnum(c) != 0) continue;
        if (std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) == std::string::npos) {
            return false;
        }
    }
    return true;
}

// Header values may contain HTAB but no other control character: CR/LF would
// inject headers, and httplib skips its own check on the multimap path we use.
bool isHeaderValue(const std::string& s) {
    for (const unsigned char c : s) {
        if (isCtl(c) && c != '\t') return false;
    }
    return true;
}

// The URL as it may appear in an error message: control bytes escaped (no
// log injection), userinfo and query/fragment redacted (they can carry
// credentials, e.g. "?key=...").
std::string redactUrl(const std::string& url) {
    std::string cut = url.substr(0, url.find_first_of("?#"));
    const bool hadQuery = cut.size() != url.size();
    const auto schemeEnd = cut.find("://");
    const std::size_t authStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    const auto authEnd = cut.find('/', authStart);
    const auto at = cut.rfind('@', authEnd == std::string::npos ? std::string::npos : authEnd);
    if (at != std::string::npos && at >= authStart) {
        cut.replace(authStart, at - authStart, "<redacted>");
    }
    std::string out;
    for (const unsigned char c : cut) {
        if (isCtl(c)) {
            static const char* hex = "0123456789ABCDEF";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
    if (hadQuery) out += "?<redacted>";
    return out;
}

SplitUrl splitUrl(const std::string& url) {
    SplitUrl out;
    // No control bytes or spaces anywhere: they are never valid in a URL, and
    // httplib's path encoder stops at the first NUL, silently truncating.
    for (const unsigned char c : url) {
        if (isCtl(c) || c == ' ') return out;
    }
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
    // apparent host).
    if (hostport.find('@') != std::string::npos) return out;
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
            out.error = "invalid url: " + redactUrl(req.url);
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

        httplib::Headers headers;
        std::string contentType = "application/json";
        for (const auto& h : req.headers) {
            // httplib validates only in Request::set_header, which the
            // multimap path below bypasses, so an unchecked name or value
            // could inject headers or smuggle a request. The error never
            // echoes the offending bytes (the value may be a secret).
            if (!isHeaderName(h.first)) {
                out.error = "invalid header name";
                return out;
            }
            if (!isHeaderValue(h.second)) {
                out.error = "invalid header value (control character): " + h.first;
                return out;
            }
            if (iequals(h.first, "content-type")) {
                contentType = h.second;
            } else {
                headers.emplace(h.first, h.second);
            }
        }

        // Pre-flight cancel comes after input validation, so a malformed
        // request is reported as such whatever the token's state.
        if (cancel.cancelled()) {
            out.error = "cancelled";
            return out;
        }

        httplib::Client cli(u.base);
        cli.set_connection_timeout(req.connectTimeout);
        cli.set_read_timeout(req.readTimeout);
        cli.set_write_timeout(req.readTimeout);
        cli.set_follow_location(false);

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
        hreq.content_receiver = [&](const char* data, std::size_t len, std::size_t,
                                    std::size_t) -> bool {
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
