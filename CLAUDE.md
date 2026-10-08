# CLAUDE.md — corvus

Guidance for AI assistants (and humans) working in this repo. Read this first.

## What this project is

**corvus** — an in-process, offline-capable, low-latency **AI agent runtime for C++**. Think "LangChain, but native C++17" — except the real moat is **local-first + embeddable + MCP-native**, not API mimicry.

> Positioning (lead with the capability, not the imitation):
> *An agent runtime that runs where Python can't — ROS2 nodes, game threads, drones, trading loops — and speaks MCP so it works with the existing tool ecosystem on day one.*

- `corvus` is the **working name** (namespace `corvus`, `#include <corvus/corvus.h>`, CMake target `corvus::corvus`). Final public name still TBD.
- **"Jarvis"** is reserved for a later *demo assistant* built on top — NOT the library.

### Goals
1. Genuinely useful, elite (top-1%) open-source contribution for C++ developers who have no LangChain equivalent.
2. Serve: ROS2 robotics, game dev (Unreal), edge/embedded (RPi/Jetson/drones), HFT/low-latency, systems programmers.
3. Earn adoption via a real problem solved obviously: 12-line quickstart, offline demos, MCP ecosystem, clean CMake integration.

### Three differentiators (folded into the design)
1. **MCP-native client** — use thousands of existing MCP servers as tools; solves ecosystem cold-start.
2. **Native tool-calling + GBNF** — provider tool-use JSON schemas for cloud; grammar-constrained JSON for local llama.cpp models. (ReAct is a fallback, not the default.)
3. **Async + streaming + cancellation** — non-blocking `runAsync`, token streaming, `CancelToken`. Required for ROS2/game targets (a blocking loop would stall them).

## Source-of-truth docs (read before large changes)
- **Start here — onboarding guides (describe the code as it is now):** [docs/README.md](docs/README.md) (index + reading paths) → [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (why) → [docs/CODE_TOUR.md](docs/CODE_TOUR.md) (every file/function, invariants, sharp edges) → [docs/HISTORY.md](docs/HISTORY.md) (eras + commits).
- **Design/roadmap:** [docs/specs/2026-06-29-jarvis-cpp-design.md](docs/specs/2026-06-29-jarvis-cpp-design.md) — architecture, phases, decisions.
- **Memory design (v2, revised 2026-10-08):** [docs/specs/2026-07-04-memory-design.md](docs/specs/2026-07-04-memory-design.md) — store vs view policies, atomic exchange commits, system prompt as agent config, cache-friendly batched trimming, sanitizer, overflow backstop, SqliteMemory sessions/schema, summary/FactStore phasing.
- **Cloud clients (Phase 1):** [docs/specs/2026-07-15-cloud-clients-design.md](docs/specs/2026-07-15-cloud-clients-design.md) (transport/error/usage contracts, retry policy) + [docs/plans/2026-07-15-cloud-clients-plan.md](docs/plans/2026-07-15-cloud-clients-plan.md) (5 PRs).
- **Historical explainers (dated snapshots, partly stale):** [docs/history/phase-0-explained.md](docs/history/phase-0-explained.md), [docs/history/pr1-http-transport-explained.md](docs/history/pr1-http-transport-explained.md).
- **Original vision:** `ai-agent orchestration architecture.html` (repo root) — the long-form design the above revises (framework-first reorder).

## Architecture (core runtime)

All units are small, single-purpose, behind stable interfaces:

| Unit | Header | Role |
|------|--------|------|
| `Message`/`ToolCall`/`CancelToken`/`ToolContext`/`ToolResult` | `include/corvus/types.h` | Shared value types. `Message` round-trips provider wire formats (assistant turns keep `toolCalls`, tool turns keep `toolCallId`). |
| `Tool` / `FunctionTool` / `makeTool` | `include/corvus/tool.h` | The agent's "hands". 3 sources — built-in, user C++, MCP — all uniform. `execute(args, ctx)` never throws; returns typed `ToolResult` (Ok/Retryable/Fatal/Timeout/Cancelled). `makeTool` has a simple string-lambda form and a context-aware form. |
| `Schema` / `schema()` | `include/corvus/schema.h` | Fluent builder → JSON Schema string, so tool authors don't hand-write JSON. |
| `Args` | `include/corvus/args.h` | Move-only view over a tool call's parsed JSON args (`parse`, `has`, strict `getString/getNumber/getInteger/getBool`). Pimpl over nlohmann/json — the json header stays in `src/`. Null = absent. |
| `typedTool<T>` / `TypedToolBuilder` | `include/corvus/typed_tool.h` | **Preferred way to write a C++ tool.** Args are a struct; `.field(key, &T::m, desc[, Optional])` derives schema + parsing from one list; `.run(fn)` → `ToolPtr`. Bad args → `RetryableError("invalid arguments: …")`. Flat fields only. |
| `ToolRegistry` | `include/corvus/tool_registry.h` | Thread-safe by-name toolbox. Duplicate names throw unless `OverwritePolicy::Replace` (anti-shadowing). |
| `Memory` / `InMemoryMemory` | `include/corvus/memory.h` | Conversation history sent back each turn (LLM is stateless). Holds conversation only — the system prompt is agent config (`withSystemPrompt`, P1). `SqliteMemory` = Phase 1. Bounding = opt-in **view** policies over the store (`lastN` P1, `maxTokens`/`autoWindow` P2, summary P3+), cutting in batches for prompt-cache hits. Gains `appendAll` (P1) so the loop commits whole exchanges atomically — see memory spec v2. |
| `LLMClient` + `ToolSpec`/`LLMResponse` | `include/corvus/llm_client.h` | Backend abstraction. Native tool-calling shape (text OR tool calls) + streaming `onToken`. |
| `Strategy` | `include/corvus/strategy.h` | `ToolCalling` (default), `ReAct` (fallback), `PlanAndExecute` (Phase 4). Builder throws on not-yet-implemented ones. |
| `Agent` (+ `AgentCallbacks`, `RunResult`) | `include/corvus/agent.h` | **The loop.** `run()` (blocking) + `runAsync()` (future + cancel). Shared-state handle: the future owns the state (destroy/move-safe mid-run); one run at a time (overlap throws `logic_error`). Loop guard via `maxIterations`. |
| `AgentBuilder` | `include/corvus/agent_builder.h` | Fluent construction with fail-fast validation. Public face of the API. Unset memory/registry default to fresh objects per `build()` (two builds = two independent agents). |
| `MockLLM` | `include/corvus/mock_llm.h` | Deterministic fake backend → offline, key-free, reproducible tests. `reply` / `callTool` / `replyAndCallTool`. |
| `HttpTransport` + `HttpRequest`/`HttpResponse`/`ChunkCallback` | `include/corvus/http_transport.h` | Seam between LLM clients and the network (raw HTTP bytes). `post()` blocks, never throws (`status == 0` = transport failure), streams via `onChunk`, honors `CancelToken` per buffer. `defaultHttpTransport()` = cpp-httplib impl in `src/httplib_transport.cpp` (URL/header guards, body caps, no redirects). |
| `MockHttpTransport` | `include/corvus/mock_http_transport.h` | Scripted transport double (FIFO responses/chunks, request recording). Must mirror the real transport exactly — pinned by `tests/test_transport_contract.cpp`. Not in the `corvus.h` umbrella (test utility). |

The agent loop (`src/agent.cpp`): build tool specs → append task to memory → loop{ check cancel → `llm.complete()` → if no tool calls, done → else record assistant turn with its tool calls, run each tool with a `ToolContext`, append id-paired observations } up to `maxIterations`.

### Design patterns in use
Strategy (LLMClient/Memory/HttpTransport/Strategy), Builder (AgentBuilder), Command (Tool), Registry (ToolRegistry), Observer (AgentCallbacks), Template Method (agent loop), Seam + Test Double (MockLLM, MockHttpTransport), RAII guard (`RunningGuard`, one run at a time), Handle/shared state (`Agent` → `State`). Each solves a concrete problem — not decoration; see [docs/ARCHITECTURE.md §4](docs/ARCHITECTURE.md#4-design-patterns-and-the-problem-each-one-solves-here).

## Directory layout
```
include/corvus/   public headers  (STABLE CONTRACT — keep dependency-light)
src/              implementations
tests/            doctest suite (MockLLM + MockHttpTransport; runs offline)
examples/         mock_quickstart (offline demo)
cmake/            corvusConfig.cmake.in (find_package support)
docs/             README (index), ARCHITECTURE, CODE_TOUR, HISTORY; specs/, plans/, history/
.github/workflows ci.yml (Linux/macOS/Windows + ASan/UBSan + TSan)
```

## Build & test

Requires **CMake ≥ 3.18** and a **C++17** compiler (MSVC 2019+, GCC ≥ 9, or Clang ≥ 10).
This machine uses **MSVC Build Tools 2026 + CMake 4.3** (verified working). **`cmake` is not on PATH here** (neither Git Bash nor PowerShell) — call the VS-bundled binary: `"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"` (also recorded in `build/CMakeCache.txt` as `CMAKE_COMMAND`). OpenSSL isn't installed locally, so local builds are TLS-less (expected CMake warning); CI covers the rest.

```bash
cmake -S . -B build -DCORVUS_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
# full doctest summary:
./build/tests/Release/corvus_tests.exe
```

Tests must stay **offline and deterministic** (MockLLM, no API keys, no network in the test path). Add a test with every behavior change.

## Conventions
- **C++17.** Namespace `corvus`. Formatting via `.clang-format` (Google base, 4-space, 100 col); lint via `.clang-tidy`.
- **Public headers are a contract** — keep them dependency-light and stable; don't churn them casually.
- **Tools never throw.** Return a typed `ToolResult` (`ok`/`retryable`/`fatal`); the loop renders errors as `"ERROR: <why>"` for the model. (`makeTool` enforces never-throw for lambdas; `Tool` subclasses must do it manually.) Blocking tools must honor `ToolContext` cancel/deadline cooperatively.
- **API-first:** design the usage site (the 12-line quickstart) before internals.
- **YAGNI:** don't build later-phase features early.
- **Dependencies:** cpp-httplib v0.18.3 + nlohmann/json v3.11.3 (since PR #1; json first used in PR 2), OpenSSL optional (`CORVUS_ENABLE_TLS`). All fetched sources-only and linked **PRIVATE** — never include a third-party header from `include/corvus/`. Adding a dependency needs a real need + a pinned tag.
- **Docs stay current:** a PR that changes a public header updates its comments **and** its entry in [docs/CODE_TOUR.md](docs/CODE_TOUR.md); a merged feature PR adds an era/entry to [docs/HISTORY.md](docs/HISTORY.md). Specs/plans are the decision record — don't rewrite them after the fact (fix broken links only). Exception: a deliberate design revision *before* the code exists may update a spec in place, marked with a `Revised:` date in the header and a revision log entry saying what changed (precedent: memory spec v2, 2026-10-08).
- Commit messages: Conventional Commits; end with the `Co-Authored-By` trailer.

## Git workflow (Phase 1 onward)
Repo: <https://github.com/Ambar-Gupta22/corvus>. `main` is the public face — **always green, always buildable**.

- **Branch per deliverable, not per phase.** One coherent feature = one short-lived `feat/<name>` branch = one PR. No giant `phase-1` branch (unreviewable diff, drift, merge pain). A phase is "done" when its PRs are merged, not when one big branch lands.
- **Every PR:** builds + tests pass on the 3-OS CI matrix before merge; includes tests (repo rule: a test with every behavior change) and doc updates it makes stale.
- **Merge style: squash** — one commit per feature on `main`, clean `git log` for visitors.
- **Trivial fixes** (typo, doc line) may go straight to `main`. PRs are for features.
- Naming: `feat/`, `fix/`, `docs/`, `ci/` prefixes.

### Phase 1 branch plan (dependency order)
| # | Branch | Delivers | Depends on |
|---|--------|----------|------------|
| 1 | `feat/http-transport` | ✅ **merged (#1)** — mockable HTTP transport seam; adds cpp-httplib + nlohmann/json | — |
| 1b | `feat/typed-tools` | ✅ **merged (#5)** — `Args` + `typedTool<T>` (struct-typed tool args, schema derived) | 1 |
| 2 | `feat/anthropic-client` | `AnthropicClient`: native tool-calling, streaming, cancel threaded in, usage (incl. cache tokens) in `LLMResponse`, prompt-cache breakpoints, same-role turn merging; loop: atomic exchange commits (`Memory::appendAll`) + `withSystemPrompt` | 1 |
| 3 | `feat/openai-client` | `OpenAIClient`, same contract | 1 |
| 4 | `feat/retries-backoff` | client retry/backoff branching on retryable vs fatal | 2 |
| 5 | `feat/per-tool-timeout` | loop deadline via `ToolContext` + watchdog net | — |
| 6 | `feat/sqlite-memory` | `SqliteMemory` (same `Memory` contract): sessions, `schema_version` + migrations, JSON rows, transactional `appendAll`, load-time repair | 2 (`appendAll`) |
| 7 | `feat/memory-trim-backstop` | view-policy seam + batched `lastN` + turn-boundary rules + sanitizer + request-level overflow backstop (learned cap) + `onContextTrim`/`RunResult` counters | 2 (error surface) |
| 8 | `feat/arg-validation` | per-call schema validation (required keys, primitive types, 64 KB cap) — reuse `Args` for parsing | 1b |
| 9 | `feat/tool-calculator` | Calculator built-in | 8 |
| 10 | `feat/toolguard-http` | `ToolGuard` primitive + guarded HttpRequest (SSRF guard: scheme allowlist, private-IP block, caps) | 1, 5 |
| 11 | `feat/usage-cost` | usage/cost surfaced in `RunResult` (`withPricing` + optional `withCachePricing`) | 2 |

Row 5 is independent — parallelize freely. Row 6 can start any time but merges after row 2 (it overrides `appendAll`). Milestone when all merged: **12-line quickstart against a real API.**

## Roadmap (framework-first; each phase = spec → plan → build)
- **Phase 0 — foundations** ✅ *(done: core loop, tools, memory, MockLLM, CI, docs)*
- **Phase 1 — cloud backends:** real Anthropic + OpenAI clients (HTTP + native tool-calling + streaming), `SqliteMemory`, retries/backoff. Adds cpp-httplib + nlohmann/json. **First built-in tools: Calculator + guarded HttpRequest** (behind a mockable HTTP transport so tests stay offline). **Per-tool timeout** in the loop (cooperative deadline/cancel via `ToolContext`, watchdog as net — mechanism in the design doc), **usage/cost in `RunResult`**, cancel threaded into the client, a `ToolGuard` per-tool safety primitive, **memory (spec v2):** system prompt as agent config (`withSystemPrompt`), atomic exchange commits (`appendAll` — no half-written history, ever), `SqliteMemory` with sessions + versioned schema + load-time repair, batched cache-friendly `lastN` view policy, transcript sanitizer, always-on request-level overflow backstop (truncate oversized msg head+tail → trim harder → retry → typed `ContextOverflow`, learned cap), observable trims (`onContextTrim`); trim rules: keep in-flight exchange, atomic tool exchanges, cut in batches — and per-call arg validation (required keys + primitive types + size cap).
- **Phase 2 — local-first:** Ollama + llama.cpp + GBNF; **jailed File I/O tools** (`read_file`/`write_file`/`list_dir`, path-allowlisted — needs only `ToolGuard`, not RBAC); **token-budget memory** (watermarked `maxTokens` with clamp + output reserve, `autoWindow`, pluggable `TokenCounter`: chars/4 estimator default, llama.cpp exact free); Raspberry Pi offline demo + benchmarks.
- **Phase 3 — MCP-native:** `McpClient` (stdio + HTTP/SSE), adapt MCP tools into the registry **namespaced as `<server>__<tool>`, with descriptions treated as untrusted input (length caps + sanitization)**. **WebSearch arrives here via a public MCP server** — not an owned integration (avoids provider coupling + key management, stays true to local-first). **`SummarizingMemory`** (opt-in decorator holding an `LLMClient`; P3+, may slip later — not launch-blocking).
- **Phase 4 — multi-agent orchestration:** Orchestrator + EventBus + routing + parallel tool/agent exec; `PlanAndExecute`; **guarded Shell** built-in (ships with the policy layer); `ToolPolicy`/RBAC (decision 8).
- **Phase 5 — flagship demos:** ROS2 planner node, game NPC; CONTRIBUTING + **SECURITY.md/disclosure policy** + good-first-issues.
- **Phase 6 — launch:** README/benchmarks/GIFs; Show HN → r/cpp → ROS → Unreal → r/raspberry_pi → llama.cpp.
- **Post-1.0 (optional):** the "Jarvis" demo assistant (CLI → voice → phone → cloud); **`FactStore`** long-term memory (separate retrieval interface + `recall_facts`/`remember_fact` tools — embeddings never enter the core lib). Off the critical path.

## Current status
**Phase 1 in progress.** Phase 0 complete, then hardened per [docs/specs/2026-07-06-phase0-hardening-design.md](docs/specs/2026-07-06-phase0-hardening-design.md) (message round-trip, tool contract v2, agent handle semantics, registry anti-shadowing, CMake install/export, TSan CI). Merged PRs: **#1** HTTP transport seam (Phase 1 branch 1), **#2** builder/mock fixes (independent agents per `build()`, unique MockLLM ids, version-sync test), **#5** typed tools (`Args` + `typedTool<T>`). **Next up: `feat/anthropic-client`** (branch 2). Verified locally: **57 test cases / 198 assertions pass** under MSVC. Backend factories (`anthropic`/`openai`/`ollama`) are still **stubs that throw** — use `MockLLM` for now. Known gaps + their fixing PRs: [docs/CODE_TOUR.md Appendix A](docs/CODE_TOUR.md#appendix-a--sharp-edges-index). Repo: <https://github.com/Ambar-Gupta22/corvus> — work lands via feature-branch PRs (see Git workflow); `main` stays green.

## Open / parked decisions (not yet finalized)
Consolidated so future sessions don't assume these are settled:

1. **Library name** — `corvus` is a **placeholder/working name**. Final public name is TBD. It drives the namespace, `include/corvus/` dir, and CMake target, so renaming later = a sed sweep. ("Jarvis" stays reserved for the demo assistant regardless.)
2. **Extension model** — leaning **MCP-only** for third-party extension. Undecided whether to also ship native in-process plugins (which would require a pure C ABI, never C++ types across the boundary).
3. **Tool contract** — ~~string in/out vs typed result~~ **RESOLVED (2026-07-06 hardening):** `execute(const std::string& args, const ToolContext& ctx) -> ToolResult` — typed status (retryable vs fatal) for the loop, `"ERROR: ..."` text for the model, context for cancel/deadline.
4. **Schema builder** — hand-rolled JSON string (dependency-free) vs rewrite on nlohmann/json once Phase 1 pulls it in. Leaning: switch to the lib for correctness. *Partly settled (2026-10-06, typed tools):* arg **parsing** uses nlohmann behind the `Args` pimpl; schema **emission** is still the hand-rolled `Schema` (typed tools reuse it).
5. ~~**Memory trimming**~~ **RESOLVED (2026-07-04, full design; v2 hardening 2026-10-08):** memory = composable opt-in **view** policies behind the `Memory` seam (additive changes only: `appendAll`) — unbounded stays default; `lastN` (P1, growth cap, NOT a fit guarantee) → `maxTokens`/`autoWindow` (P2, real fit) → `SummarizingMemory` (P3+, opt-in, only impl allowed to call an LLM) → `FactStore` (post-1.0, separate interface, not a `Memory`). Always-on request-level overflow backstop from P1. **v2 rules:** store ≠ view (backstop never mutates the store); system prompt is agent config, never in memory; memory only receives complete exchanges (atomic `appendAll`); trim in batches so provider prompt caching keeps hitting; sanitizer on every request; SqliteMemory has sessions + `schema_version` + load-time repair; trims are observable. Full rules + edge cases: [docs/specs/2026-07-04-memory-design.md](docs/specs/2026-07-04-memory-design.md).
6. **Async execution** — using `std::async` (simple); a managed thread pool is deferred until proven necessary.
7. **Branding/attribution** — repo is live at `Ambar-Gupta22/corvus`; README URLs point there. LICENSE copyright stays "corvus contributors" (fine for a community project — revisit only if a legal entity/name change demands it).
8. **Tool access control (RBAC)** — no access control in Phase 0; `ToolRegistry` is a dumb thread-safe map. Per-agent isolation works today by composition (each agent's builder gets only its allowed tools — an agent can't call a tool that isn't in its registry). A real policy layer (`ToolPolicy` / filtered-registry view, optionally tool **scopes** with execution-time denial + audit) is **Phase 4** (multi-agent orchestration). Keep the registry dumb; put policy in a thin layer above it. **`ToolPolicy` (per-agent "may this agent use this tool?") is distinct from `ToolGuard` (per-tool "is this call itself safe?", decision 9) — don't conflate them; the guard ships much earlier.**
9. **Built-in tools — phasing + safety.** Ship by dependency + blast radius, not all at once: **Calculator + guarded HttpRequest** (Phase 1), **jailed File I/O** (Phase 2), **guarded Shell** (Phase 4, with RBAC). **WebSearch is *not* owned** — comes free via MCP (Phase 3); shipping a provider integration + key management contradicts local-first and is a maintenance tax. Every dangerous tool carries a **`ToolGuard`** — a *per-tool* safety primitive (path jail / private-IP + scheme allowlist / timeout / output cap) inside `execute()`, independent of Phase-4 RBAC. **Note: HttpRequest is *not* a "safe" tool** — raw outbound HTTP from an LLM is an SSRF risk (cloud-metadata `169.254.169.254`, internal services); it ships only with its guard. Separately, the **agent loop needs a per-tool timeout** (Phase 1): today one hung `execute()` freezes the whole agent thread, which is disqualifying for the ROS2/game/HFT targets — the single highest-value gap to close.

See the 👉 notes in [docs/history/phase-0-explained.md](docs/history/phase-0-explained.md) for the reasoning behind 3–6.

## Extension model (important)
Primary extension path is **MCP** (process-isolated, no ABI risk) and **user-defined C++ tools** via `makeTool`/`Tool`. Native in-process `.so`/DLL plugins, if ever added, MUST use a **pure C ABI** (never pass `std::string`/`std::shared_ptr` across the boundary) — currently leaning MCP-only.
