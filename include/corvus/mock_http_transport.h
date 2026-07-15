#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "corvus/http_transport.h"

namespace corvus {

// MockHttpTransport — scripted test double for HttpTransport. Enqueue
// responses (or chunk sequences for streaming) in FIFO order; every request
// is recorded for wire-format assertions. Not thread-safe: drive it from the
// test thread.
class MockHttpTransport final : public HttpTransport {
public:
    struct Script {
        HttpResponse response;            // status/headers (+ body when chunks empty)
        std::vector<std::string> chunks;  // non-empty => streamed through onChunk
    };

    // Next call returns this response verbatim.
    void enqueue(HttpResponse response) { scripts_.push_back({std::move(response), {}}); }

    // Next call streams these chunks through onChunk, then reports `status`.
    void enqueueStream(int status, std::vector<std::string> chunks,
                       std::vector<std::pair<std::string, std::string>> headers = {}) {
        HttpResponse r;
        r.status = status;
        r.headers = std::move(headers);
        scripts_.push_back({std::move(r), std::move(chunks)});
    }

    // Called just before chunk `chunkIndex` is delivered — lets a test fire a
    // CancelToken or flip state mid-stream.
    std::function<void(std::size_t chunkIndex)> onBeforeChunk;

    // Every request seen, in order.
    const std::vector<HttpRequest>& requests() const { return requests_; }

    HttpResponse post(const HttpRequest& req, const ChunkCallback& onChunk,
                      const CancelToken& cancel) override {
        requests_.push_back(req);

        if (scripts_.empty()) {
            HttpResponse r;
            r.error = "MockHttpTransport: no scripted response";
            return r;
        }
        if (cancel.cancelled()) {
            HttpResponse r;
            r.error = "cancelled";  // mirror the real transport's pre-flight check
            return r;
        }
        Script s = std::move(scripts_.front());
        scripts_.pop_front();

        // Normalize the scripted payload: an enqueue()'d response carries its
        // bytes in body, an enqueueStream() carries them in chunks.
        std::vector<std::string> payload = s.chunks;
        if (payload.empty() && !s.response.body.empty()) payload.push_back(s.response.body);

        // Non-streamed call: whole payload collapses into body (real transport
        // reads the full response into HttpResponse::body).
        if (!onChunk) {
            HttpResponse r;
            r.status = s.response.status;
            r.headers = std::move(s.response.headers);
            r.body.clear();
            for (const auto& c : payload) r.body += c;
            return r;
        }

        const bool ok2xx = s.response.status / 100 == 2;

        // Non-2xx streamed reply: the real transport does NOT forward error
        // bytes to onChunk; it keeps the payload in body for the caller to
        // parse. Mirror that.
        if (!ok2xx) {
            HttpResponse r;
            r.status = s.response.status;
            r.headers = std::move(s.response.headers);
            for (const auto& c : payload) r.body += c;
            return r;
        }

        for (std::size_t i = 0; i < payload.size(); ++i) {
            if (onBeforeChunk) onBeforeChunk(i);
            if (cancel.cancelled()) {
                HttpResponse r;
                r.error = "cancelled";
                return r;
            }
            if (!onChunk(payload[i].data(), payload[i].size())) {
                HttpResponse r;
                r.error = "aborted by receiver";
                return r;
            }
        }

        HttpResponse done;
        done.status = s.response.status;
        done.headers = std::move(s.response.headers);
        return done;  // streamed 2xx success: body stays empty (see HttpResponse)
    }

private:
    std::deque<Script> scripts_;
    std::vector<HttpRequest> requests_;
};

}  // namespace corvus
