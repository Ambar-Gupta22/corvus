# Cloud client subsystem — design spec

**Date:** 2026-07-15
**Scope:** Phase 1 branches 1–4 + 11 — `feat/http-transport`, `feat/anthropic-client`, `feat/openai-client`, `feat/retries-backoff`, `feat/usage-cost`.
**Out of scope (separate specs):** per-tool timeout, SqliteMemory, memory trim/backstop, arg validation, Calculator, ToolGuard/HttpRequest.

## Goal

Replace the throwing backend stubs with real `AnthropicClient` and `OpenAIClient` implementations: native tool-calling, token streaming, sub-second cancellation, retries with backoff, and token/cost accounting — all testable offline through a mockable HTTP transport. Milestone served: the 12-line quickstart completes a multi-tool task against a real API.

## Decisions (locked during brainstorming)

1. **Seam at the HTTP boundary** (approach A). One small `HttpTransport` interface; clients own all provider knowledge (request JSON, SSE grammar, error mapping). A shared internal `SseParser` removes the only real duplication between clients. Rejected: a higher-level "provider transport" seam (Anthropic and OpenAI SSE grammars differ enough that the abstraction leaks) and localhost test servers (port/firewall flakiness; violates offline-test rule).
2. **Error contract: typed exception.** `LLMClient::complete()` throws `LLMError` (kind enum + HTTP status + provider message) after client-internal retries are exhausted. Signature unchanged; `run()` propagates; `runAsync()` rethrows at `future.get()`. The memory overflow backstop (branch 7) branches on `Kind::ContextOverflow`.
3. **Cost: tokens always, pricing user-supplied.** `RunResult` always reports token counts; dollar cost computed only when the user configures pricing. No built-in price table to go stale.
4. **OpenAI surface: Chat Completions** (`/v1/chat/completions`) — the ecosystem-standard shape (Ollama, vLLM, OpenRouter, Groq clone it), so Phase 2 local backends reuse this wire format. Responses API rejected: proprietary, no reuse.

## Components

| Unit | Location | Role |
|------|----------|------|
| `HttpRequest` / `HttpResponse` / `ChunkCallback` | `include/corvus/http_transport.h` | Value types for one HTTP exchange (std types only) |
| `HttpTransport` | same | The seam: `post(req, onChunk, cancel) -> HttpResponse` |
| `HttplibTransport` | `src/httplib_transport.cpp` | Real impl over cpp-httplib (OpenSSL TLS on all platforms) |
| `MockHttpTransport` | `include/corvus/mock_http_transport.h` | Scripted responses/chunks; records requests for assertions |
| `LLMError`, `Usage` | `include/corvus/llm_client.h` | Typed error + usage additions |
| `SseParser` | `src/sse_parser.h` (internal) | Bytes in → complete SSE events out; shared by both clients |
| `AnthropicClient` | `src/anthropic_client.cpp` | Messages API wire format |
| `OpenAIClient` | `src/openai_client.cpp` | Chat Completions wire format |
| `RetryPolicy` | `src/retry_policy.h` (internal) | Attempts, backoff + jitter, `Retry-After` |

**Dependencies added** (FetchContent, pinned release tags): cpp-httplib, nlohmann/json. Neither appears in any public header — `include/corvus/` stays dependency-light (std types only).

`MockLLM` is untouched and remains the agent-loop test backend; `MockHttpTransport` tests the clients themselves.

## Transport contract

```cpp
// include/corvus/http_transport.h
struct HttpRequest {
    std::string url;                       // full https URL
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                      // JSON, already serialized
    std::chrono::milliseconds connectTimeout{10'000};
    std::chrono::milliseconds readTimeout{120'000};   // per-read; generous for streaming
};

struct HttpResponse {
    int status = 0;                        // 0 = transport failure (no HTTP exchange)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                      // full body; empty when streamed via onChunk
    std::string error;                     // transport-level failure text (status == 0)
};

// Return false to abort the socket (cancel / early stop).
using ChunkCallback = std::function<bool(const char* data, size_t len)>;

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    // Blocking POST. If onChunk is set, body bytes stream through it as they
    // arrive and HttpResponse.body stays empty. Implementations check `cancel`
    // between chunks; an abort yields status 0 + error "cancelled".
    virtual HttpResponse post(const HttpRequest& req, const ChunkCallback& onChunk,
                              const CancelToken& cancel) = 0;
};
using HttpTransportPtr = std::shared_ptr<HttpTransport>;
```

Rules:

- The transport **never throws**; failures land in `status`/`error` so retry logic branches on values.
- The transport knows nothing of SSE, JSON, or providers — raw bytes only.
- Cancel path: the cpp-httplib content receiver returns false → socket closes → sub-second cancellation. This satisfies the design-doc requirement "thread the token into the client, abort the socket".
- POST only; `get()` is added when a consumer exists.

`MockHttpTransport`: FIFO of scripted `HttpResponse`s or chunk sequences; records every request; optional per-call hook to simulate cancellation or mid-stream failure.

## Error and usage contract

```cpp
// include/corvus/llm_client.h additions
class LLMError : public std::runtime_error {
public:
    enum class Kind {
        Auth,             // 401/403 — bad or missing key; never retried
        RateLimit,        // 429 — retried with backoff first
        Network,          // no HTTP exchange (DNS, TLS, refused, timeout)
        Server,           // 5xx / 529 overloaded — retried first
        ContextOverflow,  // prompt too long; memory backstop branches on this
        InvalidRequest,   // other 4xx — bug or bad input; never retried
        Cancelled         // CancelToken fired mid-request
    };
    LLMError(Kind kind, int httpStatus, const std::string& message);
    Kind kind() const noexcept;
    int httpStatus() const noexcept;   // 0 when no HTTP exchange happened
};

struct Usage {
    int promptTokens = 0;
    int completionTokens = 0;
};

struct LLMResponse {
    std::string text;
    std::vector<ToolCall> toolCalls;
    Usage usage;                        // zeros if provider omitted usage
};
```

Provider mapping (inside each client): 401/403 → `Auth`; 429 → `RateLimit`; 5xx and Anthropic 529 → `Server`; transport `status == 0` → `Network`. **ContextOverflow detection:** Anthropic — 400 whose message contains "prompt is too long"; OpenAI — 400 with `code == "context_length_exceeded"`. Any other 4xx → `InvalidRequest`.

Agent loop change: `Kind::Cancelled` is caught and converted to a graceful `RunResult{completed = false}` — the same semantics as today's loop-top cancel check, now effective mid-request. Every other kind propagates to the caller.

```cpp
struct RunResult {
    std::string output;
    int iterations = 0;
    bool completed = false;
    Usage usage;           // summed over every complete() call in the run
    double costUSD = 0.0;  // 0 unless pricing configured
};
```

`AgentBuilder::withPricing(double inPerMTok, double outPerMTok)` enables cost: `costUSD = promptTokens × inPerMTok / 1e6 + completionTokens × outPerMTok / 1e6`.

Deliberately omitted: a normalized `stopReason` field — no consumer yet; revisit with the memory-trim work.

## Clients

Both clients share one internal shape: build request JSON from `(messages, tools)` → `transport->post()` wrapped by `RetryPolicy` → parse response or SSE stream → `LLMResponse`, or throw `LLMError`. Streaming is used only when `onToken` is set; otherwise the plain JSON (non-stream) path is taken.

### AnthropicClient — `POST {base}/v1/messages`

- Headers: `x-api-key`, `anthropic-version: 2023-06-01`, `content-type: application/json`.
- Request: a leading `role == "system"` message becomes the top-level `system` parameter. `Message.toolCalls` → assistant `tool_use` content blocks; `role == "tool"` messages → a user turn containing a `tool_result` block carrying `toolCallId`. `ToolSpec` → `tools[]` entries with `input_schema` parsed from `parametersJson`. `max_tokens` (required by the API) comes from options, default 4096.
- Streaming events: `content_block_delta`/`text_delta` → `onToken`; `content_block_start` (tool_use) + `input_json_delta` → accumulate tool-call arguments; `message_start` + `message_delta` → usage; `error` event → mapped `LLMError`.

### OpenAIClient — `POST {base}/v1/chat/completions`

- Header: `Authorization: Bearer <key>`.
- Request: roles map 1:1. Assistant `toolCalls` → `tool_calls[]` (type `function`, arguments as a JSON string); `role == "tool"` → `tool_call_id`. `ToolSpec` → `tools[]` function entries.
- Streaming: `delta.content` → `onToken`; `delta.tool_calls` assembled by index; `stream_options: {"include_usage": true}` so the final chunk carries usage; `data: [DONE]` terminates.

### Shared internals and configuration

- `SseParser` (internal): stateful byte feeder — buffers chunks, splits events on blank lines, emits `(event, data)` pairs; handles CRLF and multi-line `data:`. Both clients drive it from their `ChunkCallback`.
- `Message` already round-trips provider wire formats (Phase 0 hardening) — no type changes.

```cpp
struct ClientOptions {
    std::string baseUrl;        // override for proxies / compatible endpoints
    int maxTokens = 4096;       // sent on every request; the API requires it for Anthropic
    int maxRetries = 3;         // retries after the initial attempt; 0 disables
    HttpTransportPtr transport; // inject a mock in tests; null = HttplibTransport
};
LLMClientPtr anthropic(const std::string& model, const std::string& key = "",
                       ClientOptions opts = {});
LLMClientPtr openai(const std::string& model, const std::string& key = "",
                    ClientOptions opts = {});
```

Empty `key` falls back to `ANTHROPIC_API_KEY` / `OPENAI_API_KEY`; a missing key throws `LLMError{Auth}` at first `complete()`. The `baseUrl` override on `OpenAIClient` gives Ollama/vLLM/OpenRouter compatibility before Phase 2 starts.

## Retry policy (internal)

Wraps each `transport->post()` in both clients:

- **Retries:** `RateLimit` (429), `Server` (5xx/529), `Network` (status 0).
- **Never retries:** `Auth`, `InvalidRequest`, `ContextOverflow`, `Cancelled` — retrying cannot help.
- **Backoff:** exponential with full jitter — `delay = rand(0, min(cap, base × 2^attempt))`, base 1 s, cap 30 s. A provider `Retry-After` header (429/529) is honored instead, capped at 60 s.
- **Cancel-aware waiting:** backoff sleeps in ~100 ms slices checking the `CancelToken`; cancellation during backoff throws `Cancelled` promptly.
- **Streaming rule:** retry only while zero bytes have reached `onToken`. After visible output, a mid-stream failure is a `Network` error with no retry — replaying would duplicate tokens.
- Exhausted retries rethrow the last `LLMError` unchanged.

`RetryPolicy` is not public API; only `ClientOptions.maxRetries` is user-visible, so the policy shape can change without an API break.

## Testing

All default-suite tests are offline and deterministic (repo rule). New files:

- **`tests/test_sse_parser.cpp`** — events split at hostile byte boundaries (mid-line, mid-UTF-8, one byte at a time), CRLF vs LF, multi-line `data:`, `[DONE]`, ignored comments/heartbeats.
- **`tests/test_anthropic_client.cpp` / `tests/test_openai_client.cpp`** — via `MockHttpTransport`:
  - Request wire format: exact JSON assertions — system extraction, tool_use/tool_result round-trip (Anthropic), tool_calls/tool_call_id (OpenAI), tools schema embedding, headers/auth.
  - Response parsing: text answer, single and parallel tool calls, usage extraction.
  - Streaming: scripted chunk sequences → `onToken` ordering, tool-argument assembly from deltas, usage from final events.
  - Error-mapping matrix: each (status, body) → expected `LLMError::Kind`, including both ContextOverflow detections.
  - Cancel: transport hook fires the token mid-stream → socket abort → `Kind::Cancelled`.
- **`tests/test_retry_policy.cpp`** — sleep and RNG injected via `std::function` hooks so tests run instantly: retries on 429/5xx/network, honors `Retry-After`, never retries the non-retryable kinds, cancel during backoff exits promptly, zero-bytes-streamed rule, gives up after `maxRetries` rethrowing the last error.
- **`tests/test_agent.cpp`** (extended) — end-to-end: real `AnthropicClient` + `MockHttpTransport` under the real `Agent` loop completes a multi-tool task offline; `RunResult.usage` summed across turns; `costUSD` math with `withPricing`; mid-run cancel yields `completed == false`.

**Known gap (accepted):** `HttplibTransport` itself (~100 lines) has no default-suite test — it is the one unmockable piece. It is exercised by `examples/real_quickstart.cpp`, a new example that runs only when `ANTHROPIC_API_KEY` is set and is never wired into CTest.

CI: existing 3-OS matrix + sanitizers unchanged; FetchContent pins exact release tags.

## Delivery

| PR | Branch | Contents |
|----|--------|----------|
| 1 | `feat/http-transport` | transport header + `HttplibTransport` + `MockHttpTransport` + deps |
| 2 | `feat/anthropic-client` | `LLMError`/`Usage` additions, `SseParser`, `AnthropicClient`, factory, loop Cancelled handling |
| 3 | `feat/openai-client` | `OpenAIClient` + factory (parallel with 4 after 2 merges the error types) |
| 4 | `feat/retries-backoff` | `RetryPolicy` wired into both clients |
| 5 | `feat/usage-cost` | `RunResult` usage/cost aggregation + `withPricing` |

Each PR: green 3-OS CI, tests included, squash-merged. Milestone when all merged: quickstart against a real API.
