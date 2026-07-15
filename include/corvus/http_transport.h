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
    // Full body for non-streamed calls (capped; an over-cap body fails as a
    // transport error rather than growing unbounded). For a streamed 2xx the
    // body goes through onChunk and this stays empty. For a streamed non-2xx
    // the error bytes are NOT sent to onChunk — they are kept here (capped) so
    // the caller can parse the provider's error payload.
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
    // branch on values. If onChunk is set, 2xx body bytes stream through it as
    // they arrive. `cancel` is checked before the request and on every received
    // buffer; a fired token aborts the socket and yields status 0 + error
    // "cancelled" (a fully silent server is still bounded by readTimeout — the
    // agent loop's watchdog is the backstop for a wedged non-cooperative peer).
    virtual HttpResponse post(const HttpRequest& req, const ChunkCallback& onChunk,
                              const CancelToken& cancel) = 0;
};

using HttpTransportPtr = std::shared_ptr<HttpTransport>;

// The production transport (cpp-httplib). HTTPS works when the library was
// built with OpenSSL (CORVUS_ENABLE_TLS); otherwise https URLs fail with a
// clear transport error.
HttpTransportPtr defaultHttpTransport();

}  // namespace corvus
