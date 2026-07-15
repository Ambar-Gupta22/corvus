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
        Script s = std::move(scripts_.front());
        scripts_.pop_front();

        // Non-streamed call: chunks (if scripted) collapse into the body,
        // mirroring a real transport reading the full payload.
        if (s.chunks.empty() || !onChunk) {
            for (const auto& c : s.chunks) s.response.body += c;
            return std::move(s.response);
        }

        for (std::size_t i = 0; i < s.chunks.size(); ++i) {
            if (onBeforeChunk) onBeforeChunk(i);
            if (cancel.cancelled()) {
                HttpResponse r;
                r.error = "cancelled";
                return r;
            }
            if (!onChunk(s.chunks[i].data(), s.chunks[i].size())) {
                HttpResponse r;
                r.error = "aborted by receiver";
                return r;
            }
        }

        HttpResponse done;
        done.status = s.response.status;
        done.headers = std::move(s.response.headers);
        return done;  // streamed success: body stays empty (see HttpResponse)
    }

private:
    std::deque<Script> scripts_;
    std::vector<HttpRequest> requests_;
};

}  // namespace corvus
