# jarvis-cpp — Strategy, Design & Roadmap (framework-first)

> Working name `jarvis-cpp` is a placeholder (final library name TBD). "Jarvis" is reserved as the name of the later **demo assistant**, not the library.

## Context

This is an open-source AI agent **framework for C++**. The original source-of-truth design doc is `ai-agent orchestration architecture.html` in the repo root — it positions the project as "LangChain for C++": a native C++17 agent runtime (Tool, Registry, Memory, Task, EventBus, ReasoningStrategy + ReAct loop + multi-agent orchestration), backends for Anthropic/OpenAI/Ollama/llama.cpp, a `.so` plugin system, CMake FetchContent packaging, and target audiences in ROS2 robotics, game dev, edge/embedded, and HFT.

**Problem with that roadmap:** it built a personal desktop assistant (voice → phone → ngrok → Oracle cloud) *first*, then extracted the library. That buries the actual product (the framework) under a kitchen-sink assistant. The goal is an **elite, top-1%, genuinely useful OSS contribution** — so the assistant should be *a* demo of the framework, not a prerequisite.

**This revision:** reorder to **framework-first**, demote the personal assistant to an optional post-1.0 showcase, and raise the engineering bar with three differentiators the original plan lacked.

## Decisions locked

- **Scope:** Framework-only first. Personal "Jarvis" assistant (voice/phone/cloud) → optional showcase **after** v1.0. Off the critical path.
- **Differentiators folded in:** (1) **MCP-native client**, (2) **native tool-calling + GBNF grammar-constrained JSON** for local models, (3) **async + streaming + cancellation**. (Tracing/eval not a headline phase — covered cheaply by the step-callback hook.)
- **Name:** decide later; `jarvis-cpp` is placeholder.

## Positioning (the reframe)

> **An in-process, offline-capable, low-latency AI agent runtime for C++ — it runs where Python can't (ROS2 nodes, game threads, drones, trading loops) and speaks MCP so it works with the existing tool ecosystem on day one.**

"LangChain for C++" stays as the one-line discovery hook, but the moat is **local-first + embeddable + MCP-native**, not API mimicry.

### Why the three differentiators matter
- **MCP-native** solves the ecosystem cold-start. Rather than waiting for the community to write `jarvis-gmail.so`, an MCP client lets jarvis-cpp use *thousands of existing MCP servers* (GitHub, Slack, filesystem, DBs) with zero custom code — instantly useful, wider audience, and process isolation sidesteps the plugin-ABI problem.
- **Native tool-calling + GBNF.** ReAct text-parsing is fragile/dated. Correctness = provider tool-use JSON schemas (Anthropic/OpenAI) for cloud, and **GBNF grammar-constrained decoding** to force valid tool-call JSON from local llama.cpp models.
- **Async + streaming + cancellation.** A blocking `agent.run()` would stall a ROS2 spin loop or a game frame. Need `runAsync()` (future + cancel token), token-streaming callbacks, timeouts.

### Engineering correction
The original `extern "C" Tool* create_tool()` plugin seam returns a C++ vtable object and passes `std::string`/`std::shared_ptr` across `.so` boundaries — UB across compiler/STL versions. **Fix:** MCP is the *primary* extension path (process-isolated); native in-process plugins, if kept, use a **pure C ABI**, documented as same-toolchain only. (Leaning MCP-only.)

## Architecture — core runtime (v0.x library)

1. **LLM backend** — `LLMClient` interface. Impls: `AnthropicClient`, `OpenAIClient`, `OllamaClient`, `LlamaCppClient` (behind CMake flag). Each supports chat completion, **native tool-calling** (send tool JSON schemas, receive structured tool calls), **streaming** (token callback), and for local backends **GBNF** grammar constraint. Factories: `anthropic()`, `openai()`, `ollama()`, `llamaCpp()`.
2. **Tool** — `name()`, `description()`, `inputSchema()` (JSON schema), `execute(args) -> Result`. Rule: never throw; return an error result. `ToolRegistry`: thread-safe register/lookup + schema export. **One registry, three uniform sources** — all are `Tool` instances, treated identically by the agent:
   - **Built-in tools** ship with the library: WebSearch, Calculator, HttpRequest, sandboxed File I/O, guarded Shell.
   - **User-defined C++ tools** — the primary in-process extension path. Two authoring styles: (a) subclass `Tool` for stateful/complex tools; (b) **`makeTool(name, desc, schema, lambda)`** `FunctionTool` adapter for the common case — zero subclassing, a `std::function<std::string(const json&)>` body. A small **schema helper** (`schema().str(...).num(...)`) generates `inputSchema()` so users never hand-write JSON schema.
   - **MCP tools** — adapted into `Tool` via `McpClient`.
   - Registration is **explicit** by default (`.withTool(...)` / `registry.registerTool(...)`) — predictable, no hidden control flow. An optional `JARVIS_REGISTER_TOOL(MyTool)` static self-registration macro is offered for power users, documented as advanced (global init-order caveat across the `dlopen` boundary).
3. **MCP** — `McpClient` (stdio + HTTP/SSE). Discovers a server's tools and adapts each into a `Tool`, uniform to the agent.
4. **Memory** — `Memory`: append, get-context (token-budget windowing + pluggable summarization), persist. Impls: `InMemoryMemory`, `SqliteMemory`.
5. **Reasoning strategy** — Strategy pattern: `ToolCalling` (native function-calling loop — default), `ReAct` (text fallback), `PlanAndExecute`. Template-method loop with hooks.
6. **Agent runtime** — `Agent` owns client/registry/memory/strategy/limits. API: `run(task)` (blocking convenience), `runAsync(task) -> std::future<Result>` with `CancelToken` + timeout + `maxIterations`, and a callback form (`onToken`, `onToolCall`, `onToolResult`, `onStep`) that doubles as the lightweight tracing hook. Fluent `AgentBuilder`. Retries with backoff; infinite-loop guard.
7. **Orchestration (multi-agent)** — `Orchestrator` routes tasks (Chain of Responsibility), `EventBus` (Observer) for agent-to-agent reactions, parallel execution via thread pool. *Later phase.*

**Cross-cutting:** config (keys via env), structured logging behind the step callback, semantic versioning, stable public API in `include/jarvis/jarvis.h`.

**Deps (light):** cpp-httplib (header-only), nlohmann/json, sqlite3; llama.cpp optional behind CMake flag. MIT license.

**Quality bar:** GitHub Actions CI Linux/macOS/Windows × gcc/clang/msvc; ASan/UBSan/TSan; clang-format + clang-tidy; doctest/Catch2 tests driven by a deterministic **MockLLM** (no API key); CMake FetchContent now, vcpkg/Conan later; doc site.

## Roadmap (framework-first). Each phase = its own spec → plan → build cycle.

- **Phase 0 — Foundations (Wk 1):** repo skeleton, CMake, CI, license, format/tidy, test harness + MockLLM, public header.
- **Phase 1 — Core single-agent + cloud (Wk 2–4):** `LLMClient` + Anthropic (+OpenAI); `ToolCalling` strategy + ReAct fallback; Tool/Registry + built-in tools + **`makeTool` lambda adapter + schema helper** (user-defined tool path); Memory (InMemory + Sqlite); `Agent`/`AgentBuilder`; `run` + `runAsync` + cancel + streaming; retries + loop guard; tests. **Milestone: 12-line quickstart works against a real API.**
- **Phase 2 — Local-first (Wk 5–6):** Ollama + llama.cpp backends; **GBNF** tool-call JSON; benchmark vs Python cold start; **RPi-5 fully-offline demo**.
- **Phase 3 — MCP-native (Wk 7–8):** `McpClient` (stdio + HTTP/SSE) + tool adaptation. Demo: agent using an off-the-shelf MCP server with **zero custom tool code**.
- **Phase 4 — Multi-agent orchestration (Wk 9–10):** Orchestrator + EventBus + routing + parallel exec; `PlanAndExecute`.
- **Phase 5 — Flagship demos + extension (Wk 11–12):** ROS2 planner node; game-NPC; optional native **C-ABI** plugin example; `CONTRIBUTING.md`; 5 good-first-issues.
- **Phase 6 — Launch (Wk 13–15):** README (benchmarks, GIFs, comparison); CI badge; MIT; v1.0.0 criteria; Show HN → r/cpp → ROS Discourse → Unreal → r/raspberry_pi → llama.cpp discussions; dev.to blog.
- **Later / optional (post-1.0):** personal **Jarvis assistant** showcase — CLI → whisper.cpp voice → REST/phone → cloud.

### Defaults
- **C++17 baseline** (compat with ROS2/Unreal/embedded). Async via `std::future` + thread pool; optional C++20 coroutine adapter later.
- **Two flagship demos committed:** RPi-5 offline + ROS2 planner. Game-NPC is a strong third, not a gate.

## Verification (per phase)
- **Unit/CI:** MockLLM-driven tests for agent loop, tool dispatch, memory windowing, retries/loop-guard; sanitizers clean; CI green on 3 OSes.
- **Phase 1:** 12-line quickstart against a real Anthropic key completes a multi-tool task.
- **Phase 2:** local model emits GBNF-valid tool JSON; capture RPi-5 offline run + cold-start/memory benchmarks.
- **Phase 3:** connect to a public MCP server, list tools, complete a task with no jarvis-specific tool code.
- **Phase 4:** orchestrator runs ≥2 agents in parallel; EventBus delivers cross-agent events; reproducible under MockLLM.
- **Phase 5/6:** each example builds on a fresh machine in <5 min; README benchmarks reproduce; CI badge green.

## Open items
- Final library name (parked).
- Native in-process plugins vs **MCP-only** extension (leaning MCP-only).
