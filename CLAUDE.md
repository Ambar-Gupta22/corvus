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
- **Phase 0 explainer (plain language + rationale):** [docs/phase-0-explained.md](docs/phase-0-explained.md).
- **Original vision:** `ai-agent orchestration architecture.html` (repo root) — the long-form design the above revises (framework-first reorder).

## Architecture (core runtime)

All units are small, single-purpose, behind stable interfaces:

| Unit | Header | Role |
|------|--------|------|
| `Tool` / `FunctionTool` / `makeTool` | `include/corvus/tool.h` | The agent's "hands". 3 sources — built-in, user C++, MCP — all uniform. Never throw; return `"ERROR: ..."`. |
| `Schema` / `schema()` | `include/corvus/schema.h` | Fluent builder → JSON Schema string, so tool authors don't hand-write JSON. |
| `ToolRegistry` | `include/corvus/tool_registry.h` | Thread-safe by-name toolbox. |
| `Memory` / `InMemoryMemory` | `include/corvus/memory.h` | Conversation history sent back each turn (LLM is stateless). `SqliteMemory` = Phase 1. |
| `LLMClient` + `ToolCall`/`ToolSpec`/`LLMResponse` | `include/corvus/llm_client.h` | Backend abstraction. Native tool-calling shape (text OR tool calls) + streaming `onToken`. |
| `Strategy` | `include/corvus/strategy.h` | `ToolCalling` (default), `ReAct` (fallback), `PlanAndExecute` (Phase 4). |
| `Agent` (+ `CancelToken`, `AgentCallbacks`, `RunResult`) | `include/corvus/agent.h` | **The loop.** `run()` (blocking) + `runAsync()` (future + cancel). Loop guard via `maxIterations`. |
| `AgentBuilder` | `include/corvus/agent_builder.h` | Fluent construction with fail-fast validation. Public face of the API. |
| `MockLLM` | `include/corvus/mock_llm.h` | Deterministic fake backend → offline, key-free, reproducible tests. |

The agent loop (`src/agent.cpp`): build tool specs → append task to memory → loop{ check cancel → `llm.complete()` → if no tool calls, done → else run each tool, append observations } up to `maxIterations`.

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

Requires **CMake ≥ 3.16** and a **C++17** compiler (MSVC 2019+, GCC ≥ 9, or Clang ≥ 10).
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
- **Tools never throw.** Return `"ERROR: <why>"`. (`makeTool` enforces this for lambdas; `Tool` subclasses must do it manually.)
- **API-first:** design the usage site (the 12-line quickstart) before internals.
- **YAGNI:** don't build later-phase features early. Keep Phase 0 dependency-free until Phase 1 genuinely needs JSON/HTTP libs.
- Commit messages: Conventional Commits; end with the `Co-Authored-By` trailer.

## Roadmap (framework-first; each phase = spec → plan → build)
- **Phase 0 — foundations** ✅ *(done: core loop, tools, memory, MockLLM, CI, docs)*
- **Phase 1 — cloud backends:** real Anthropic + OpenAI clients (HTTP + native tool-calling + streaming), `SqliteMemory`, retries/backoff. Adds cpp-httplib + nlohmann/json. **First built-in tools: Calculator + guarded HttpRequest** (behind a mockable HTTP transport so tests stay offline). **Per-tool timeout** in the loop, **usage/cost in `RunResult`**, cancel threaded into the client, and a `ToolGuard` per-tool safety primitive.
- **Phase 2 — local-first:** Ollama + llama.cpp + GBNF; **jailed File I/O tools** (`read_file`/`write_file`/`list_dir`, path-allowlisted — needs only `ToolGuard`, not RBAC); Raspberry Pi offline demo + benchmarks.
- **Phase 3 — MCP-native:** `McpClient` (stdio + HTTP/SSE), adapt MCP tools into the registry. **WebSearch arrives here via a public MCP server** — not an owned integration (avoids provider coupling + key management, stays true to local-first).
- **Phase 4 — multi-agent orchestration:** Orchestrator + EventBus + routing + parallel tool/agent exec; `PlanAndExecute`; **guarded Shell** built-in (ships with the policy layer); `ToolPolicy`/RBAC (decision 8).
- **Phase 5 — flagship demos:** ROS2 planner node, game NPC; CONTRIBUTING + good-first-issues.
- **Phase 6 — launch:** README/benchmarks/GIFs; Show HN → r/cpp → ROS → Unreal → r/raspberry_pi → llama.cpp.
- **Post-1.0 (optional):** the "Jarvis" demo assistant (CLI → voice → phone → cloud). Off the critical path.

## Current status
Phase 0 complete and verified locally: **12 test cases / 35 assertions pass** under MSVC. Backend factories (`anthropic`/`openai`/`ollama`) are **stubs that throw** until Phase 1 — use `MockLLM` for now. Not yet pushed to GitHub.

## Open / parked decisions (not yet finalized)
Consolidated so future sessions don't assume these are settled:

1. **Library name** — `corvus` is a **placeholder/working name**. Final public name is TBD. It drives the namespace, `include/corvus/` dir, and CMake target, so renaming later = a sed sweep. ("Jarvis" stays reserved for the demo assistant regardless.)
2. **Extension model** — leaning **MCP-only** for third-party extension. Undecided whether to also ship native in-process plugins (which would require a pure C ABI, never C++ types across the boundary).
3. **Tool contract** — `execute` takes/returns `std::string` (simple, zero header deps) vs a typed result struct (`success` flag + payload). Chose string for now. **Phase 1 sub-decision:** the flat `"ERROR: ..."` string can't tell retryable from fatal — pick a typed result *or* an `ERROR:RETRY` / `ERROR:FATAL` convention so retries/backoff can branch on it.
4. **Schema builder** — hand-rolled JSON string (dependency-free) vs rewrite on nlohmann/json once Phase 1 pulls it in. Leaning: switch to the lib for correctness.
5. **Memory trimming** — history currently grows unbounded; needs a strategy before real context limits bite. "Keep last N" vs summarize old turns. Undecided.
6. **Async execution** — using `std::async` (simple); a managed thread pool is deferred until proven necessary.
7. **Branding/attribution** — LICENSE copyright is "corvus contributors" (placeholder); README has a `<you>` GitHub-URL placeholder. Fill in on first push.
8. **Tool access control (RBAC)** — no access control in Phase 0; `ToolRegistry` is a dumb thread-safe map. Per-agent isolation works today by composition (each agent's builder gets only its allowed tools — an agent can't call a tool that isn't in its registry). A real policy layer (`ToolPolicy` / filtered-registry view, optionally tool **scopes** with execution-time denial + audit) is **Phase 4** (multi-agent orchestration). Keep the registry dumb; put policy in a thin layer above it. **`ToolPolicy` (per-agent "may this agent use this tool?") is distinct from `ToolGuard` (per-tool "is this call itself safe?", decision 9) — don't conflate them; the guard ships much earlier.**
9. **Built-in tools — phasing + safety.** Ship by dependency + blast radius, not all at once: **Calculator + guarded HttpRequest** (Phase 1), **jailed File I/O** (Phase 2), **guarded Shell** (Phase 4, with RBAC). **WebSearch is *not* owned** — comes free via MCP (Phase 3); shipping a provider integration + key management contradicts local-first and is a maintenance tax. Every dangerous tool carries a **`ToolGuard`** — a *per-tool* safety primitive (path jail / private-IP + scheme allowlist / timeout / output cap) inside `execute()`, independent of Phase-4 RBAC. **Note: HttpRequest is *not* a "safe" tool** — raw outbound HTTP from an LLM is an SSRF risk (cloud-metadata `169.254.169.254`, internal services); it ships only with its guard. Separately, the **agent loop needs a per-tool timeout** (Phase 1): today one hung `execute()` freezes the whole agent thread, which is disqualifying for the ROS2/game/HFT targets — the single highest-value gap to close.

See the 👉 notes in [docs/phase-0-explained.md](docs/phase-0-explained.md) for the reasoning behind 3–6.

## Extension model (important)
Primary extension path is **MCP** (process-isolated, no ABI risk) and **user-defined C++ tools** via `makeTool`/`Tool`. Native in-process `.so`/DLL plugins, if ever added, MUST use a **pure C ABI** (never pass `std::string`/`std::shared_ptr` across the boundary) — currently leaning MCP-only.
