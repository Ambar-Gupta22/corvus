# Cloud client subsystem — implementation plan

**Spec:** [2026-07-15-cloud-clients-design.md](../specs/2026-07-15-cloud-clients-design.md)
**Delivery:** 5 PRs, dependency order below. Every PR: green 3-OS CI + sanitizers, tests included, squash-merge, doc updates it makes stale.

---

## PR 1 — `feat/http-transport`

**Goal:** the mockable seam + dependency onboarding. No provider code yet.

1. **CMake: dependencies.** FetchContent for cpp-httplib and nlohmann/json, pinned to exact release tags. `CPPHTTPLIB_OPENSSL_SUPPORT` on all platforms; CI installs OpenSSL where missing (Windows: vcpkg or choco). Link deps into `corvus` PRIVATE — nothing leaks into public headers or the install interface beyond what exists today.
2. **`include/corvus/http_transport.h`** — `HttpRequest`, `HttpResponse`, `ChunkCallback`, `HttpTransport`, `HttpTransportPtr` exactly as specced (std types only; includes `corvus/types.h` for `CancelToken`).
3. **`src/httplib_transport.cpp`** — `HttplibTransport`: URL split (scheme/host/port/path), header pass-through, connect/read timeouts from the request, non-stream and content-receiver (streaming) paths, cancel checked in the receiver (return false → abort → `{0, "cancelled"}`), all failures mapped to `status`/`error` values. Never throws.
4. **`include/corvus/mock_http_transport.h`** — scripted FIFO of responses or chunk sequences, request recording, optional per-call hook.
5. **Tests: `tests/test_transport_contract.cpp`** — MockHttpTransport behavior itself (scripting, recording, chunk delivery, hook-driven cancel) so later client tests stand on verified ground. No network.
6. **CI check:** all 3 OS build with the new deps; sanitizer jobs still pass. Update CLAUDE.md dep note if wording goes stale.

**Acceptance:** library builds with deps on 3 OSes; mock transport drives a fake streamed exchange in tests; public headers still std-only.

---

## PR 2 — `feat/anthropic-client`

**Goal:** first real backend + the error/usage contract everything downstream branches on.

1. **`include/corvus/llm_client.h` additions** — `LLMError` (kind, httpStatus, message), `Usage`, `LLMResponse.usage`, `ClientOptions`, new factory overloads. Keep existing signatures intact.
2. **`src/sse_parser.h/.cpp`** (internal) — stateful feeder: buffer bytes → emit `(event, data)` pairs; blank-line event split, CRLF/LF, multi-line `data:`, comment/heartbeat skip.
3. **Tests first for the parser: `tests/test_sse_parser.cpp`** — hostile chunk boundaries (1-byte feeds, mid-UTF-8), CRLF, multi-line data, `[DONE]` passthrough.
4. **`src/anthropic_client.cpp`** — request build (system extraction, tool_use/tool_result mapping, tools with `input_schema`, `max_tokens`), non-stream parse, streaming via SseParser (text deltas → onToken, input_json_delta accumulation, usage from message_start/message_delta, error events), error mapping incl. "prompt is too long" → `ContextOverflow`. Wire `anthropic()` factory: env-key fallback, default `HttplibTransport`, `ClientOptions.transport` injection. Remove the Anthropic stub from `src/clients_stub.cpp`.
5. **Agent loop** (`src/agent.cpp`) — catch `LLMError` with `kind()==Cancelled` → `RunResult{completed=false}`; everything else propagates.
6. **Tests: `tests/test_anthropic_client.cpp`** — wire-format exact-JSON assertions, response/streaming parsing, error matrix, cancel mid-stream, usage extraction (per spec test list). Extend `tests/test_agent.cpp`: AnthropicClient + MockHttpTransport end-to-end multi-tool run offline; mid-run cancel.
7. **Docs:** CLAUDE.md status line (anthropic stub → real); quickstart docs still Mock-based until PR 5.

**Acceptance:** offline end-to-end agent run through real client code; error matrix green; loop cancel semantics preserved.

---

## PR 3 — `feat/openai-client` (after PR 2; parallel with PR 4)

1. **`src/openai_client.cpp`** — Chat Completions request build (1:1 roles, tool_calls/tool_call_id), non-stream parse, streaming (delta.content, index-assembled delta.tool_calls, `stream_options.include_usage`, `[DONE]`), error mapping incl. `context_length_exceeded` → `ContextOverflow`. Factory + env fallback + `baseUrl` override. Remove OpenAI stub.
2. **Tests: `tests/test_openai_client.cpp`** — same categories as Anthropic tests.
3. **Docs:** note `baseUrl` → Ollama/OpenRouter compatibility.

**Acceptance:** same bar as PR 2 for the OpenAI wire format.

---

## PR 4 — `feat/retries-backoff` (after PR 2; parallel with PR 3)

1. **`src/retry_policy.h/.cpp`** (internal) — attempt loop, retryable-kind classification, exponential backoff with full jitter (base 1 s, cap 30 s), `Retry-After` honor (cap 60 s), sliced cancel-aware sleep, zero-bytes-streamed rule, injectable sleep + RNG hooks.
2. **Wire into both clients** (AnthropicClient always; OpenAIClient in whichever of PR 3/4 merges second — coordinate the rebase). `ClientOptions.maxRetries` honored.
3. **Tests: `tests/test_retry_policy.cpp`** — per spec list, instant via injected hooks. Client-level test: scripted 429 then 200 → succeeds with one retry; 401 → no retry.

**Acceptance:** deterministic retry tests; no real sleeps in the suite; cancel exits backoff promptly.

---

## PR 5 — `feat/usage-cost`

1. **`include/corvus/agent.h`** — `RunResult.usage`, `RunResult.costUSD`. **`agent_builder.h/.cpp`** — `withPricing(inPerMTok, outPerMTok)`; plumb pricing into `Agent::State`.
2. **`src/agent.cpp`** — sum `LLMResponse.usage` per `complete()` call into the run's `RunResult`; compute cost when pricing set.
3. **Tests:** extend `tests/test_agent.cpp` — multi-turn usage summation, cost math, zero-cost default. MockLLM gains optional per-reply usage so this stays offline.
4. **`examples/real_quickstart.cpp`** — 12-line quickstart against real Anthropic API; built with examples, runs only when `ANTHROPIC_API_KEY` set; never in CTest.
5. **Docs sweep:** CLAUDE.md status → "Phase 1 cloud client subsystem merged"; README quickstart shows the real-API snippet; design spec status notes.

**Acceptance:** milestone check — quickstart completes a multi-tool task against the real API (manual, keyed); `RunResult` reports tokens + cost; full suite green offline.

---

## Cross-cutting rules

- **TDD bias:** wire-format and parser tests written against the spec before/with the implementation; error matrix is the contract.
- **No public-header churn beyond spec:** additions only (`LLMError`, `Usage`, `ClientOptions`, `RunResult` fields, `withPricing`). Existing user code keeps compiling.
- **Windows first-class:** MSVC + OpenSSL path validated in CI from PR 1 — do not defer TLS-on-Windows pain to PR 5.
- **Each PR leaves `main` releasable:** stubs remain for not-yet-implemented backends (ollama throws until Phase 2).

## Risks

| Risk | Mitigation |
|------|------------|
| OpenSSL on Windows CI (setup pain, slow builds) | Tackle in PR 1 where the diff is small; cache the install |
| Provider SSE format drift | Error matrix + parser tests document assumptions; `anthropic-version` pinned |
| PR 3/4 touch same client files | Declare merge order at PR-open time; second one rebases |
| cpp-httplib header-only compile-time cost | Compile it in exactly one TU (`httplib_transport.cpp`) |
