#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "corvus/types.h"

namespace corvus {

// HttpTransport — the seam between the LLM clients and the network. Clients
// own everything provider-specific (request JSON, SSE grammar, error mapping);
// the transport moves raw bytes. Tests inject MockHttpTransport here so the
// whole client stack is provable offline.

struct HttpRequest {
    std::string url;   // full URL, e.g. "https://api.anthropic.com/v1/messages"
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;  // already-serialized payload (JSON)
    std::chrono::milliseconds connectTimeout{10'000};
    std::chrono::milliseconds readTimeout{120'000};  // per-read; generous for streaming
};

struct HttpResponse {
    int status = 0;  // HTTP status; 0 = transport failure (no HTTP exchange)
    std::vector<std::pair<std::string, std::string>> headers;
    // Full body for non-streamed calls. For streamed calls the body goes
    // through onChunk instead and this stays empty — except on a non-2xx
    // status, where the (capped) received bytes are kept here so the caller
    // can parse the provider's error payload.
    std::string body;
    std::string error;  // transport-level failure text; set only when status == 0
};

// Streaming sink: receives body bytes as they arrive. Return false to abort
// the underlying socket (the cancel / early-stop path).
using ChunkCallback = std::function<bool(const char* data, std::size_t len)>;

class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    // Blocking POST. Never throws — failures land in status/error so callers
    // branch on values. If onChunk is set, body bytes stream through it as
    // they arrive. Implementations check `cancel` between chunks; a fired
    // token aborts the socket and yields status 0 + error "cancelled".
    virtual HttpResponse post(const HttpRequest& req, const ChunkCallback& onChunk,
                              const CancelToken& cancel) = 0;
};

using HttpTransportPtr = std::shared_ptr<HttpTransport>;

// The production transport (cpp-httplib). HTTPS works when the library was
// built with OpenSSL (CORVUS_ENABLE_TLS); otherwise https URLs fail with a
// clear transport error.
HttpTransportPtr defaultHttpTransport();

}  // namespace corvus
