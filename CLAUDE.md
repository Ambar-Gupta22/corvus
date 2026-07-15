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
- **Design/roadmap:** [docs/specs/2026-06-29-jarvis-cpp-design.md](docs/specs/2026-06-29-jarvis-cpp-design.md) — architecture, phases, decisions.
- **Memory design:** [docs/specs/2026-07-04-memory-design.md](docs/specs/2026-07-04-memory-design.md) — composable trimming policies, overflow backstop, summary/FactStore phasing.
- **Phase 0 explainer (plain language + rationale):** [docs/phase-0-explained.md](docs/phase-0-explained.md).
- **Original vision:** `ai-agent orchestration architecture.html` (repo root) — the long-form design the above revises (framework-first reorder).

## Architecture (core runtime)

All units are small, single-purpose, behind stable interfaces:

| Unit | Header | Role |
|------|--------|------|
| `Message`/`ToolCall`/`CancelToken`/`ToolContext`/`ToolResult` | `include/corvus/types.h` | Shared value types. `Message` round-trips provider wire formats (assistant turns keep `toolCalls`, tool turns keep `toolCallId`). |
| `Tool` / `FunctionTool` / `makeTool` | `include/corvus/tool.h` | The agent's "hands". 3 sources — built-in, user C++, MCP — all uniform. `execute(args, ctx)` never throws; returns typed `ToolResult` (Ok/Retryable/Fatal/Timeout/Cancelled). `makeTool` has a simple string-lambda form and a context-aware form. |
| `Schema` / `schema()` | `include/corvus/schema.h` | Fluent builder → JSON Schema string, so tool authors don't hand-write JSON. |
| `ToolRegistry` | `include/corvus/tool_registry.h` | Thread-safe by-name toolbox. Duplicate names throw unless `OverwritePolicy::Replace` (anti-shadowing). |
| `Memory` / `InMemoryMemory` | `include/corvus/memory.h` | Conversation history sent back each turn (LLM is stateless). `SqliteMemory` = Phase 1. Bounding = opt-in composable policies behind this same seam (`lastN` P1, `maxTokens`/`autoWindow` P2, summary P3+) — see memory spec. |
| `LLMClient` + `ToolSpec`/`LLMResponse` | `include/corvus/llm_client.h` | Backend abstraction. Native tool-calling shape (text OR tool calls) + streaming `onToken`. |
| `Strategy` | `include/corvus/strategy.h` | `ToolCalling` (default), `ReAct` (fallback), `PlanAndExecute` (Phase 4). Builder throws on not-yet-implemented ones. |
| `Agent` (+ `AgentCallbacks`, `RunResult`) | `include/corvus/agent.h` | **The loop.** `run()` (blocking) + `runAsync()` (future + cancel). Shared-state handle: the future owns the state (destroy/move-safe mid-run); one run at a time (overlap throws `logic_error`). Loop guard via `maxIterations`. |
| `AgentBuilder` | `include/corvus/agent_builder.h` | Fluent construction with fail-fast validation. Public face of the API. |
| `MockLLM` | `include/corvus/mock_llm.h` | Deterministic fake backend → offline, key-free, reproducible tests. `reply` / `callTool` / `replyAndCallTool`. |

The agent loop (`src/agent.cpp`): build tool specs → append task to memory → loop{ check cancel → `llm.complete()` → if no tool calls, done → else record assistant turn with its tool calls, run each tool with a `ToolContext`, append id-paired observations } up to `maxIterations`.

### Design patterns in use
Strategy (LLMClient/Memory/Strategy), Builder (AgentBuilder), Command (Tool), Registry (ToolRegistry), Observer (AgentCallbacks), Template Method (agent loop). Each solves a concrete problem — not decoration.

## Directory layout
```
include/corvus/   public headers  (STABLE CONTRACT — keep dependency-light)
src/              implementations
tests/            doctest suite (uses MockLLM; runs offline)
examples/         mock_quickstart (offline demo)
docs/             specs + explainers
.github/workflows ci.yml (Linux/macOS/Windows + ASan/UBSan)
```

## Build & test

Requires **CMake ≥ 3.18** and a **C++17** compiler (MSVC 2019+, GCC ≥ 9, or Clang ≥ 10).
This machine uses **MSVC Build Tools 2026 + CMake 4.3** (verified working).

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
- **YAGNI:** don't build later-phase features early. Keep Phase 0 dependency-free until Phase 1 genuinely needs JSON/HTTP libs.
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
| 1 | `feat/http-transport` | mockable HTTP transport seam; adds cpp-httplib + nlohmann/json | — |
| 2 | `feat/anthropic-client` | `AnthropicClient`: native tool-calling, streaming, cancel threaded in, usage in `LLMResponse` | 1 |
| 3 | `feat/openai-client` | `OpenAIClient`, same contract | 1 |
| 4 | `feat/retries-backoff` | client retry/backoff branching on retryable vs fatal | 2 |
| 5 | `feat/per-tool-timeout` | loop deadline via `ToolContext` + watchdog net | — |
| 6 | `feat/sqlite-memory` | `SqliteMemory` (persistent, same `Memory` contract) | — |
| 7 | `feat/memory-trim-backstop` | `lastN` policy + turn-boundary rules + overflow backstop | 2 (error surface) |
| 8 | `feat/arg-validation` | per-call schema validation (required keys, primitive types, 64 KB cap) | 1 (json lib) |
| 9 | `feat/tool-calculator` | Calculator built-in | 8 |
| 10 | `feat/toolguard-http` | `ToolGuard` primitive + guarded HttpRequest (SSRF guard: scheme allowlist, private-IP block, caps) | 1, 5 |
| 11 | `feat/usage-cost` | usage/cost surfaced in `RunResult` | 2 |

Rows 5/6 are independent — parallelize freely. Milestone when all merged: **12-line quickstart against a real API.**

## Roadmap (framework-first; each phase = spec → plan → build)
- **Phase 0 — foundations** ✅ *(done: core loop, tools, memory, MockLLM, CI, docs)*
- **Phase 1 — cloud backends:** real Anthropic + OpenAI clients (HTTP + native tool-calling + streaming), `SqliteMemory`, retries/backoff. Adds cpp-httplib + nlohmann/json. **First built-in tools: Calculator + guarded HttpRequest** (behind a mockable HTTP transport so tests stay offline). **Per-tool timeout** in the loop (cooperative deadline/cancel via `ToolContext`, watchdog as net — mechanism in the design doc), **usage/cost in `RunResult`**, cancel threaded into the client, a `ToolGuard` per-tool safety primitive, **memory: `lastN` trimming + always-on context-overflow backstop** (truncate oversized msg → trim harder → retry → clean error; trim rules: keep system msg, keep in-flight exchange, atomic tool exchanges — see memory spec), and per-call arg validation (required keys + primitive types + size cap).
- **Phase 2 — local-first:** Ollama + llama.cpp + GBNF; **jailed File I/O tools** (`read_file`/`write_file`/`list_dir`, path-allowlisted — needs only `ToolGuard`, not RBAC); **token-budget memory** (`maxTokens` with clamp + output reserve, `autoWindow`, pluggable `TokenCounter`: chars/4 estimator default, llama.cpp exact free); Raspberry Pi offline demo + benchmarks.
- **Phase 3 — MCP-native:** `McpClient` (stdio + HTTP/SSE), adapt MCP tools into the registry **namespaced as `<server>__<tool>`, with descriptions treated as untrusted input (length caps + sanitization)**. **WebSearch arrives here via a public MCP server** — not an owned integration (avoids provider coupling + key management, stays true to local-first). **`SummarizingMemory`** (opt-in decorator holding an `LLMClient`; P3+, may slip later — not launch-blocking).
- **Phase 4 — multi-agent orchestration:** Orchestrator + EventBus + routing + parallel tool/agent exec; `PlanAndExecute`; **guarded Shell** built-in (ships with the policy layer); `ToolPolicy`/RBAC (decision 8).
- **Phase 5 — flagship demos:** ROS2 planner node, game NPC; CONTRIBUTING + **SECURITY.md/disclosure policy** + good-first-issues.
- **Phase 6 — launch:** README/benchmarks/GIFs; Show HN → r/cpp → ROS → Unreal → r/raspberry_pi → llama.cpp.
- **Post-1.0 (optional):** the "Jarvis" demo assistant (CLI → voice → phone → cloud); **`FactStore`** long-term memory (separate retrieval interface + `recall_facts`/`remember_fact` tools — embeddings never enter the core lib). Off the critical path.

## Current status
Phase 0 complete, then hardened per [docs/specs/2026-07-06-phase0-hardening-design.md](docs/specs/2026-07-06-phase0-hardening-design.md) (message round-trip, tool contract v2, agent handle semantics, registry anti-shadowing, CMake install/export, TSan CI). Verified locally: **23 test cases / 76 assertions pass** under MSVC. Backend factories (`anthropic`/`openai`/`ollama`) are **stubs that throw** until Phase 1 — use `MockLLM` for now. **Pushed to GitHub: <https://github.com/Ambar-Gupta22/corvus>** — from Phase 1 onward, work lands via feature-branch PRs (see Git workflow below); `main` stays green.

## Open / parked decisions (not yet finalized)
Consolidated so future sessions don't assume these are settled:

1. **Library name** — `corvus` is a **placeholder/working name**. Final public name is TBD. It drives the namespace, `include/corvus/` dir, and CMake target, so renaming later = a sed sweep. ("Jarvis" stays reserved for the demo assistant regardless.)
2. **Extension model** — leaning **MCP-only** for third-party extension. Undecided whether to also ship native in-process plugins (which would require a pure C ABI, never C++ types across the boundary).
3. **Tool contract** — ~~string in/out vs typed result~~ **RESOLVED (2026-07-06 hardening):** `execute(const std::string& args, const ToolContext& ctx) -> ToolResult` — typed status (retryable vs fatal) for the loop, `"ERROR: ..."` text for the model, context for cancel/deadline.
4. **Schema builder** — hand-rolled JSON string (dependency-free) vs rewrite on nlohmann/json once Phase 1 pulls it in. Leaning: switch to the lib for correctness.
5. ~~**Memory trimming**~~ **RESOLVED (2026-07-04, full design):** memory = composable opt-in policies behind the unchanged `Memory` seam — unbounded stays default; `lastN` (P1, growth cap, NOT a fit guarantee) → `maxTokens`/`autoWindow` (P2, real fit) → `SummarizingMemory` (P3+, opt-in, only impl allowed to call an LLM) → `FactStore` (post-1.0, separate interface, not a `Memory`). Always-on overflow backstop from P1. Full rules + edge cases: [docs/specs/2026-07-04-memory-design.md](docs/specs/2026-07-04-memory-design.md).
6. **Async execution** — using `std::async` (simple); a managed thread pool is deferred until proven necessary.
7. **Branding/attribution** — repo is live at `Ambar-Gupta22/corvus`; README URLs point there. LICENSE copyright stays "corvus contributors" (fine for a community project — revisit only if a legal entity/name change demands it).
8. **Tool access control (RBAC)** — no access control in Phase 0; `ToolRegistry` is a dumb thread-safe map. Per-agent isolation works today by composition (each agent's builder gets only its allowed tools — an agent can't call a tool that isn't in its registry). A real policy layer (`ToolPolicy` / filtered-registry view, optionally tool **scopes** with execution-time denial + audit) is **Phase 4** (multi-agent orchestration). Keep the registry dumb; put policy in a thin layer above it. **`ToolPolicy` (per-agent "may this agent use this tool?") is distinct from `ToolGuard` (per-tool "is this call itself safe?", decision 9) — don't conflate them; the guard ships much earlier.**
9. **Built-in tools — phasing + safety.** Ship by dependency + blast radius, not all at once: **Calculator + guarded HttpRequest** (Phase 1), **jailed File I/O** (Phase 2), **guarded Shell** (Phase 4, with RBAC). **WebSearch is *not* owned** — comes free via MCP (Phase 3); shipping a provider integration + key management contradicts local-first and is a maintenance tax. Every dangerous tool carries a **`ToolGuard`** — a *per-tool* safety primitive (path jail / private-IP + scheme allowlist / timeout / output cap) inside `execute()`, independent of Phase-4 RBAC. **Note: HttpRequest is *not* a "safe" tool** — raw outbound HTTP from an LLM is an SSRF risk (cloud-metadata `169.254.169.254`, internal services); it ships only with its guard. Separately, the **agent loop needs a per-tool timeout** (Phase 1): today one hung `execute()` freezes the whole agent thread, which is disqualifying for the ROS2/game/HFT targets — the single highest-value gap to close.

See the 👉 notes in [docs/phase-0-explained.md](docs/phase-0-explained.md) for the reasoning behind 3–6.

## Extension model (important)
Primary extension path is **MCP** (process-isolated, no ABI risk) and **user-defined C++ tools** via `makeTool`/`Tool`. Native in-process `.so`/DLL plugins, if ever added, MUST use a **pure C ABI** (never pass `std::string`/`std::shared_ptr` across the boundary) — currently leaning MCP-only.
