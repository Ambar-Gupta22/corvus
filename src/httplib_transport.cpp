#include "corvus/http_transport.h"

#ifdef CORVUS_HAS_TLS
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace corvus {
namespace {

// On a streamed non-2xx response we keep a capped copy of the bytes so the
// caller can parse the provider's error payload (see HttpResponse::body).
constexpr std::size_t kErrorBodyCap = 256 * 1024;

struct SplitUrl {
    std::string base;  // "https://host[:port]"
    std::string path;  // "/v1/messages" ("/" if absent)
    bool https = false;
    bool ok = false;
};

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
            if (iequals(h.first, "content-type")) {
                contentType = h.second;
            } else {
                headers.emplace(h.first, h.second);
            }
        }

        bool cancelled = false;
        bool aborted = false;
        std::string tee;  // capped copy for non-2xx error payloads

        httplib::Result res = [&] {
            if (!onChunk) return cli.Post(u.path, headers, req.body, contentType);
            // No Post overload takes a ContentReceiver directly; go through
            // send() with a Request carrying one.
            httplib::Request hreq;
            hreq.method = "POST";
            hreq.path = u.path;
            hreq.headers = headers;
            hreq.body = req.body;
            hreq.set_header("Content-Type", contentType);
            hreq.content_receiver = [&](const char* data, std::size_t len, std::uint64_t,
                                        std::uint64_t) -> bool {
                if (cancel.cancelled()) {
                    cancelled = true;
                    return false;  // closes the socket — sub-second cancel
                }
                if (tee.size() < kErrorBodyCap) {
                    tee.append(data, std::min(len, kErrorBodyCap - tee.size()));
                }
                if (!onChunk(data, len)) {
                    aborted = true;
                    return false;
                }
                return true;
            };
            return cli.send(hreq);
        }();

        if (!res) {
            if (cancelled || cancel.cancelled()) {
                out.error = "cancelled";
            } else if (aborted) {
                out.error = "aborted by receiver";
            } else {
                out.error = httplib::to_string(res.error());
            }
            return out;  // status stays 0
        }

        out.status = res->status;
        out.headers.reserve(res->headers.size());
        for (const auto& h : res->headers) out.headers.emplace_back(h.first, h.second);
        if (!onChunk) {
            out.body = res->body;
        } else if (out.status / 100 != 2) {
            out.body = std::move(tee);
        }
        return out;
    }
};

}  // namespace

HttpTransportPtr defaultHttpTransport() { return std::make_shared<HttplibTransport>(); }

}  // namespace corvus
