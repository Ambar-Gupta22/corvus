# corvus history

*How the codebase got to where it is, era by era: the goal, what changed, the key decisions and
their reasons, and the commits. Run `git show <hash>` on any hash to see the actual change. For
what the code looks like **now**, read [CODE_TOUR.md](CODE_TOUR.md).*

**Test-suite growth at a glance**

| Point in time | Test cases | Assertions |
|---|---|---|
| Phase 0 scaffold (`d8905d9`) | 12 | 35 |
| After hardening (`25a4559`) | 23 | 76 |
| After PR #1, HTTP transport (`1882816`) | 36 | 124 |
| After PR #2, builder/mock fixes (`7b771d5`) | 41 | 135 |

---

## Era 1 — Origins: from "Jarvis assistant" to "framework first" (June 29 – July 3, 2026)

**Goal.** Decide what to build. The starting idea (the long-form `ai-agent orchestration
architecture.html` in the repo root) was a personal "Jarvis" assistant. The key reframe: C++ has no
LangChain equivalent, so build the **library** first and keep Jarvis as a later demo built on it.

**Key decisions and why**
- **Framework first, assistant later.** A reusable library is a bigger contribution than one app,
  and the app becomes the best demo of the library.
- **Lead with capability, not imitation.** The pitch is "runs where Python can't, speaks MCP", not
  "LangChain in C++".
- **Three differentiators** (MCP-native, native tool-calling + GBNF, async + cancellation) chosen
  because each answers a concrete need of the target domains (ROS2, games, edge, HFT).
- **Working name `corvus`**; "Jarvis" reserved for the demo.
- **Tool access control is a Phase 4 concern**, kept out of the registry (decision 8).

**Commits**
- `9ae6c9e` docs: framework-first strategy, design & roadmap
  ([design spec](specs/2026-06-29-jarvis-cpp-design.md))
- `50717ce`, `8f513cf`, `a14cf3a`, `6aaea50`: CLAUDE.md project guide, consolidated open decisions,
  the RBAC decision, architecture refinements

---

## Era 2 — Phase 0: foundations (June 29 – July 1, 2026)

**Goal.** A working, dependency-free core: the agent loop, tools, memory, a fake model for tests,
and CI. Real backends are deliberately out of scope.

**What landed** (`0c32cac`, 32 files)
- Public headers for every core unit: `Tool` + `makeTool`, `Schema`, `ToolRegistry`, `Memory` +
  `InMemoryMemory`, `LLMClient`, `Strategy`, `Agent` (`run` + `runAsync` + `CancelToken`),
  `AgentBuilder`, `MockLLM`, and the `corvus.h` umbrella.
- A working ToolCalling loop with the `maxIterations` guard and cooperative cancellation.
- Backend factories (`anthropic`/`openai`/`ollama`) as stubs that throw.
- CMake target `corvus::corvus`, the doctest suite, the offline `mock_quickstart` example.
- CI: Linux/macOS/Windows build + test, plus an ASan/UBSan job.
- `.clang-format`, `.clang-tidy`, MIT license, README, CONTRIBUTING.

**Follow-up fix** (`d8905d9`): builds under CMake 4 + MSVC. doctest is fetched sources-only because
its own CMake declares a minimum version CMake 4 rejects, and MSVC's stricter transitive includes
needed explicit `<stdexcept>`. Added the Phase 0 explainer (now archived at
[history/phase-0-explained.md](history/phase-0-explained.md)).

**Key decisions and why**
- **Native tool calling as the default shape** (`LLMResponse` = text OR tool calls). Parsing
  "Action:" out of prose is fragile; ReAct becomes a fallback.
- **MockLLM from day one**, so every behavior is testable offline and reproducibly.
- **No dependencies in Phase 0.** YAGNI: nothing needed JSON or HTTP yet.

**Housekeeping in this window:** `3700fd0`, `3b64ffa`, `93c5e31` (graphify knowledge-graph tooling
set up, then its generated output and vendored skills untracked), `fedf96a` (README badges),
`5078339` (HTML design docs marked as documentation for GitHub's language stats).

---

## Era 3 — Phase 0 hardening (July 6–7, 2026)

**Goal.** Before anyone adopted the library, fix the public API promises Phase 1 couldn't keep.
Changing a public header after adoption breaks users; changing it before is free.
([hardening spec](specs/2026-07-06-phase0-hardening-design.md), `4da5cba`)

**What changed, step by step**

1. **`types.h` created** (`25a4559`): the shared value types moved into one leaf header, which also
   removed a circular include.
2. **A1: the transcript round-trips provider formats.** `Message` gained `toolCalls` (assistant
   turns) and `toolCallId` (tool turns); `ToolCall` gained `id`. The loop now always records the
   assistant turn *with* its tool calls, even when its text is empty. *Why:* Anthropic/OpenAI
   reject tool results whose request isn't in the history, so Phase 0 memory could not have been
   replayed to a real API.
3. **A2: tool contract v2.** `execute(args)` → `std::string` became `execute(args, ToolContext)` →
   `ToolResult`. *Why:* there was no way to pass a deadline or cancel signal into a tool (C++ can't
   kill threads), and `"ERROR: …"` strings couldn't express retryable vs fatal. Both needed the
   same signature change, so it was done once. `makeTool` kept a simple string form.
4. **A3: `Agent` became a safe handle.** State moved behind a `shared_ptr` that the `runAsync`
   future owns, so destroying the `Agent` mid-run no longer crashes; overlapping runs throw
   `logic_error` via `RunningGuard`.
5. **B4–B7: behavior fixes.** An early stop returns the last assistant text instead of only a
   sentinel; unimplemented strategies are rejected loudly instead of silently running ToolCalling;
   `maxIterations < 1` is rejected; the schema escaper handles every control character.
6. **C1: no silent tool shadowing** (`2ca1970`). `registerTool` throws on duplicate names unless
   `OverwritePolicy::Replace`. *Why:* groundwork for Phase 3, where MCP servers register tools
   whose names we don't control.
7. **D1: good citizen as a dependency** (`3bcd4ce`). Tests default on only for top-level builds;
   install rules + `corvusConfig.cmake` make `find_package(corvus)` work.
8. **D2: ThreadSanitizer CI job** (`39e9c10`), given real work by the new async/lifetime tests.

**Breaking changes:** the `Tool::execute` signature, the `Message` fields, `registerTool` throwing on
duplicates. All happened before the first public push, so no user was broken.

**Docs sync:** `f05e4d4`, `33ac0de`.

---

## Era 4 — Memory design (July 14, 2026)

**Goal.** Decide how conversation memory stays bounded, before Phase 1 hits real context limits.
([memory spec](specs/2026-07-04-memory-design.md), `96a37ca`, `8216f0a`)

**Key decisions and why**
- **Composable, opt-in policies behind the unchanged `Memory` seam.** Unbounded stays the default,
  because silently dropping history surprises users.
- **Phasing:** `lastN` (P1, a growth cap, *not* a fit guarantee) → `maxTokens` / `autoWindow` (P2,
  real fit, pluggable token counter) → `SummarizingMemory` (P3+, the only memory allowed to call an
  LLM) → `FactStore` (post-1.0, a separate retrieval interface).
- **Always-on overflow backstop** from Phase 1: on a provider "context too long" error, truncate the
  oversized message → trim harder → retry → fail with a clean error. Proactive policies reduce
  overflow; only a reactive backstop can eliminate it.
- **Trim rules:** keep the system message, keep the in-flight exchange, and never split a tool
  exchange.

---

## Era 5 — Phase 1 planning and the PR workflow (July 14–15, 2026)

**Goal.** Move from direct commits to reviewed PRs, and plan the cloud-client subsystem.

- `f35475d`: repo published at <https://github.com/Ambar-Gupta22/corvus>. Rules adopted: `main`
  always green; one branch per deliverable; squash merge; the Phase 1 branch table (11 branches,
  in dependency order) recorded in CLAUDE.md.
- `a926ae9`: [cloud clients design spec](specs/2026-07-15-cloud-clients-design.md) covering
  transport contract, error/usage contract, both clients, and retry policy.
- `8bb15ef`: [implementation plan](plans/2026-07-15-cloud-clients-plan.md), split into 5 PRs.

---

## Era 6 — PR #1: the HTTP transport seam (July 15, 2026)

**Commit:** `1882816` ([PR #1](https://github.com/Ambar-Gupta22/corvus/pull/1)).
**Plain-language explainer:** [history/pr1-http-transport-explained.md](history/pr1-http-transport-explained.md).

**The problem.** Cloud clients must do real HTTPS in production, but tests must never touch the
network.

**The decision: cut at raw HTTP bytes.** HTTP semantics are stable while provider formats churn, so
the contract sits on stable ground and everything provider-specific stays above it. Rejected: a
shared "provider event" layer (Anthropic and OpenAI stream differently, so it would leak) and a
localhost test server (flaky CI).

**Step by step, file by file** (10 files, +659 lines)

1. `include/corvus/http_transport.h` (new): `HttpRequest`, `HttpResponse` (`status == 0` = network
   failure), `ChunkCallback` (return false = abort), and `HttpTransport::post`, which is blocking,
   never throws, and cancellable. Standard types only.
2. `src/httplib_transport.cpp` (new): the cpp-httplib implementation, the only file that sees
   httplib.
3. `include/corvus/mock_http_transport.h` (new): the scripted double, with request recording and a
   per-chunk hook.
4. `tests/test_transport_contract.cpp` (new): 13 cases pinning mock/real parity; real-transport
   cases fail before connecting.
5. `CMakeLists.txt`: first dependencies (cpp-httplib v0.18.3, nlohmann/json v3.11.3), fetched
   sources-only and linked `PRIVATE`; optional TLS via OpenSSL; Windows socket libraries; the CMake
   minimum raised from 3.16 to 3.18.
6. `cmake/corvusConfig.cmake.in`: re-finds OpenSSL for static consumers.
7. `tests/CMakeLists.txt`, `README.md`, `CONTRIBUTING.md`, `CLAUDE.md`: wiring and doc updates.

**What the security-focused review caught, and the fixes**
1. **Mock drifted from the real transport** (the worst finding): streamed error bodies and
   plain-body + `onChunk` behaved differently. The mock was made status-aware and parity tests were
   added. This mattered because four future PRs test against this mock.
2. **`post()` could throw** `bad_alloc` on a huge body. Fixed with the 64 MB cap.
3. **Cancel didn't work mid-request** on the non-streamed path. Fixed: all bytes now go through the
   receiver, which checks the token on every buffer.
4. **CMake minimum was wrong** (3.16 declared, 3.18 required).
5. **Header injection:** CR/LF in header values is now rejected.
6. **URL userinfo attack:** `host@evil` authorities are now rejected.

**What it unlocks:** PR 2 (`AnthropicClient`), PR 3 (`OpenAIClient`), PR 4 (retries), and PR 5
(usage/cost), all tested through `MockHttpTransport`.

`7c74c75`: the PR 1 explainer, plus a README overhaul.

---

## Era 7 — PR #2: builder and mock fixes (September 29, 2026)

**Commit:** `7b771d5` ([PR #2](https://github.com/Ambar-Gupta22/corvus/pull/2)). Found while mapping
the codebase for these onboarding docs.

1. **`AgentBuilder::build()` twice** used to throw, because the first build stored its default
   registry in the builder and the second re-registered the same tools. Defaults are now locals,
   so each build gives an independent agent. Re-registering the *identical* tool object into a
   shared registry is a no-op, and a different tool with the same name still throws.
2. **`MockLLM` ids** now come from a monotonic counter, so they no longer repeat after
   consume/re-enqueue.
3. **The strategy error message** no longer promises ReAct "in Phase 1".
4. **New test** keeps the `CORVUS_VERSION_*` macros equal to the CMake project version.

Behavior change: only item 1, and only where the old behavior threw. No signatures changed.
5 new tests; 4 of them were confirmed to fail on the old code.

---

## Era 8 — Onboarding docs (September 29, 2026)

This guide: [docs index](README.md), [ARCHITECTURE.md](ARCHITECTURE.md),
[CODE_TOUR.md](CODE_TOUR.md), this file, and comment-only additions to the public headers. The two
earlier explainers moved to [history/](history/) as dated snapshots.
([spec](specs/2026-09-29-onboarding-docs-design.md))

---

## Era 9 — Typed tools (October 6, 2026)

**Commit:** `b02aaf9` ([PR #5](https://github.com/Ambar-Gupta22/corvus/pull/5)). ([spec](specs/2026-10-06-typed-tools-design.md))

Writing a C++ tool used to mean hand-parsing a raw JSON string while keeping a separate `schema()`
chain in sync with that parsing. Now a tool's args are a struct:
`typedTool<T>(name, desc).field("x", &T::x, "desc")...run(fn)`. Each `field()` call produces both
the schema entry and the parsing, so the two cannot drift. Bad args reach the model as a retryable
`ERROR: invalid arguments: <why>`.

Underneath sits `corvus::Args`, a move-only view over the parsed args. It is the first public use
of nlohmann/json, kept behind a pimpl so no third-party header enters `include/corvus/`.
`feat/arg-validation` is expected to reuse it.

Additive only: no existing signature changed. The quickstart, README and CONTRIBUTING now lead with
the typed form. 16 new tests (57 cases total).

---

## Era 10 — Memory design v2 (October 8, 2026)

**Commit:** `200f2e3` ([PR #6](https://github.com/Ambar-Gupta22/corvus/pull/6)). Docs only.

**Goal.** Before any memory code is written, check the v1 memory design against what production
agents actually hit. ([memory spec, revised in place](specs/2026-07-04-memory-design.md#revision-log))

**Key decisions and why**
- **System prompt is agent config** (`withSystemPrompt`), not a memory message. `clear()` can't
  wipe it, policies don't special-case it, and saved sessions never carry a stale one.
- **Atomic exchange commits.** Once real clients can throw, appending the user task before the
  call (as the loop does today) would leave half-written exchanges, which `SqliteMemory` would
  then save forever. The loop now buffers each exchange and commits it with one `appendAll`.
  Lands in `feat/anthropic-client`, the PR where `complete()` starts throwing.
- **Store vs view.** The store keeps; a pure view policy decides what's sent. The backstop shapes
  the outgoing request and never edits the store.
- **Batched, cache-friendly trimming.** A one-message sliding window changes the request prefix
  every call and defeats provider prompt caching. Policies cut in jumps (high/low watermarks),
  and the Anthropic client places `cache_control` breakpoints. `Usage` gains cache token fields.
- **Production persistence.** `SqliteMemory` gets sessions, a versioned schema, JSON rows, and
  load-time repair: each of these is a breaking schema change if added later.
- **Observable trims** (`onContextTrim`, `RunResult` counters), a transcript sanitizer, and a
  backstop that learns a size cap and ends in the typed `ContextOverflow` error (matching the
  cloud error contract).

---

## What's next

The remaining Phase 1 branches (Anthropic and OpenAI clients, retries, per-tool timeout, SQLite
memory, trimming + backstop, arg validation, Calculator, `ToolGuard` + HttpRequest, usage/cost) are
tracked in the branch table in [CLAUDE.md](../CLAUDE.md#phase-1-branch-plan-dependency-order). When
one merges, add an era here.
