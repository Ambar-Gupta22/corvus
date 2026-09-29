# PR 1 explained — the HTTP transport seam

*Plain-language companion to [PR #1](https://github.com/Ambar-Gupta22/corvus/pull/1). What changed, why it changed, and what the review caught. If you want the formal contracts, read the [design spec](specs/2026-07-15-cloud-clients-design.md); this file is the "explain it like I'm new here" version.*

---

## The problem this PR solves

Phase 1 gives corvus real cloud backends: `AnthropicClient` and `OpenAIClient` talking to actual APIs over HTTPS. That creates a tension:

- The clients **must** do real network I/O in production.
- Our tests **must never** touch the network (repo rule: offline, deterministic, no API keys).

If the clients called the network directly, we could only test them against live APIs — flaky, slow, costs money, needs secrets in CI. The classic fix is a **seam**: cut the stack at a well-chosen boundary, put an interface there, and swap in a fake for tests.

This PR builds that seam and nothing else. No Anthropic code, no JSON parsing, no retries — just the boundary those things will stand on. Small PR, but every later Phase 1 PR leans on it.

## Where we cut: the HTTP boundary

We considered three places to put the seam (see "Decisions" in the spec):

1. **At the HTTP boundary** (chosen) — the interface speaks raw HTTP: URL, headers, body in; status, headers, bytes out. It knows *nothing* about Anthropic, OpenAI, JSON, or SSE.
2. At a higher "provider event" level — rejected: Anthropic and OpenAI stream in different grammars, so a shared abstraction would leak immediately.
3. No seam, spin up a localhost test server — rejected: real sockets in tests mean port conflicts, firewall prompts, CI flakiness.

Why the HTTP boundary is right: **HTTP semantics never change; provider wire formats change all the time.** A seam is a contract, and you want contracts on stable ground. Everything volatile (request JSON shapes, streaming event grammars, error formats) lives *above* the seam in the clients, where it can churn freely without touching the interface.

## The files, one by one

### `include/corvus/http_transport.h` — the contract (new)

Defines four small things:

- **`HttpRequest`** — what to send: full URL, headers, an already-serialized body, and two timeouts (connect, per-read). Plain values, no behavior.
- **`HttpResponse`** — what came back: HTTP `status`, headers, `body`, and an `error` string. The rule that makes error handling sane: **`status == 0` means "the network itself failed"** (DNS, refused connection, timeout) and `error` says why. Any real HTTP exchange — even a 500 — gives you the actual status. Callers branch on values, never on exceptions.
- **`ChunkCallback`** — a function you hand in to receive body bytes *as they arrive* (streaming). Returning `false` from it aborts the socket. That one bit is how "stop generating" reaches the network layer.
- **`HttpTransport`** — the interface itself: one method, `post(request, onChunk, cancelToken)`. Blocking, and it **never throws**.

Why never-throw? The retry logic coming in PR 4 wants to look at a result and decide "retryable or not." That's a value judgment — much cleaner over a struct than over a dozen exception types flying through it.

The header includes only std types + `corvus/types.h`. That's deliberate: public headers are corvus's stable contract, and cpp-httplib must never leak into them. If we ever swap HTTP libraries, no user code recompiles differently.

### `src/httplib_transport.cpp` — the real implementation (new)

The production transport, built on [cpp-httplib](https://github.com/yhirose/cpp-httplib). All library-specific code is confined to this single file. What it does per call:

1. **Parses the URL** into base + path, rejecting anything that isn't `http(s)://`, anything with a `user@host` trick in it, and anything with control characters in the authority (more on why below).
2. **Validates headers** — any header name or value containing `\r` or `\n` is rejected before it gets near a socket.
3. **Sends the POST** with the caller's timeouts, redirects disabled.
4. **Routes the response by status**: on a 2xx with a streaming callback, bytes flow to `onChunk` as they arrive; on a non-2xx, bytes are kept (capped) in `body` so the client can parse the provider's error JSON; on a non-streamed call, the whole body lands in `body` — capped at 64 MB so a hostile server can't balloon our memory.
5. **Checks the cancel token on every received buffer** — a fired token closes the socket, and the caller gets `status 0, error "cancelled"`.

TLS: HTTPS works when the library was built with OpenSSL (`CORVUS_ENABLE_TLS`, on by default when OpenSSL is found). Built without it, an https URL fails immediately with a clear message instead of a confusing socket error. Certificate and hostname verification are on by default in the httplib version we pin — we verified this in the library source during review.

### `include/corvus/mock_http_transport.h` — the test double (new)

`MockHttpTransport` is what tests inject instead of the real thing. You script it: "next call returns this 200 with this body," or "next call streams these five chunks then reports 429." It records every request it sees, so a test can assert byte-for-byte what a client *would have sent* over the wire — no network anywhere.

The golden rule of this file: **the mock must behave exactly like the real transport.** Every branch — pre-flight cancel, 2xx streaming, non-2xx error bodies, mid-stream aborts — mirrors the real implementation. If the mock drifts, offline tests start certifying behavior that production doesn't have, and the whole Phase 1 test strategy quietly rots. (The review caught exactly this — see below.)

### `tests/test_transport_contract.cpp` — proving the contract (new)

Offline tests for both transports: scripted responses come back in order, requests are recorded, streamed chunks arrive in sequence, aborting mid-stream works, cancellation works, and the real transport's guards (bad URLs, CRLF headers, pre-fired cancel) reject without touching the network. 13 new test cases; the suite is now 36 cases / 124 assertions, still zero network.

### `CMakeLists.txt` — dependencies arrive (modified)

Phase 0 was deliberately dependency-free. Phase 1 needs two libraries, both fetched automatically at configure time, pinned to exact versions:

- **cpp-httplib v0.18.3** — the HTTP engine, compiled into exactly one file.
- **nlohmann/json v3.11.3** — JSON, used starting in PR 2.

Both are pulled "sources only" — we never run their own build scripts, so nothing of theirs pollutes what `make install` ships. This trick requires CMake ≥ 3.18, so the minimum version was raised from 3.16 (on older CMake the flag is silently ignored and the pollution happens — silent wrongness is worse than a version bump).

### `cmake/corvusConfig.cmake.in` — install correctness (modified)

If corvus was built with TLS, apps linking the *installed* static library also need OpenSSL at link time. This file now tells CMake to find it for them automatically. Without this, `find_package(corvus)` would succeed and then the final link would fail with cryptic OpenSSL symbol errors.

## What the security review caught (and the fixes)

Before merge, the PR went through an in-depth review with a security focus. Good news first: TLS certificate + hostname verification are on by default, no proxy env-var surprises, redirects disabled (a key-bearing request can't be bounced to an attacker's host). The real findings:

1. **The mock had drifted from the real transport** (worst finding, subtle). Streamed error responses returned an empty body in the mock but a populated one for real; and a scripted plain response ignored the streaming callback entirely. Fixed by making the mock status-aware and adding parity tests. This mattered more than any single bug because *four future PRs* test against this mock.
2. **`post()` could secretly throw.** An endpoint declaring a multi-gigabyte body would have caused an out-of-memory exception to escape a function documented never to throw. Fixed with the 64 MB cap → clean transport error.
3. **Cancel didn't actually work mid-request.** The token was checked once before sending, then never again on the non-streamed path — a cancel could block for the full 120 s read timeout. Fixed by routing *all* responses through the byte receiver, which checks the token on every buffer.
4. **CMake minimum was wrong** (3.16 declared, 3.18 required) — see above.
5. **Header injection guard.** A `\r\n` smuggled inside a header value would have been written raw onto the wire, letting it inject extra headers or even a second request. Not exploitable today (we control all headers), but the Phase-1 `HttpRequest` *tool* will let model-influenced values reach this transport — cheap to lock now, expensive to retrofit.
6. **URL authority guard.** `https://api.anthropic.com@evil.example/...` actually connects to `evil.example` (everything before `@` is "userinfo") — with a valid TLS cert for the attacker's own host, so TLS wouldn't save you. The API key would be sent to the attacker. Now rejected outright.

The pattern in 5 and 6: the transport doesn't trust its inputs *even though today's only caller is our own code*, because tomorrow's callers include tool code influenced by an LLM. Guards are cheapest at the choke point.

## What this unlocks

With the seam in place, the rest of the cloud client subsystem proceeds ([plan](plans/2026-07-15-cloud-clients-plan.md)):

- **PR 2** — `AnthropicClient`: real wire format, SSE streaming, typed errors (`LLMError`), token usage. Tested entirely through `MockHttpTransport`.
- **PR 3** — `OpenAIClient`, same treatment.
- **PR 4** — retries with backoff, wrapped around this transport.
- **PR 5** — usage/cost in `RunResult`; the 12-line quickstart runs against a real API.
