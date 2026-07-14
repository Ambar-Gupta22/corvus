# jarvis-cpp — Strategy, Design & Roadmap (framework-first)

> Working name: **`corvus`** (namespace `corvus`, `#include <corvus/corvus.h>`, CMake target `corvus::corvus`). Final public name still TBD but `corvus` is the working identity in code. "Jarvis" is reserved for the later **demo assistant**, not the library.

## Context

This is an open-source AI agent **framework for C++**. The original source-of-truth design doc is `ai-agent orchestration architecture.html` in the repo root — it positions the project as "LangChain for C++": a native C++17 agent runtime (Tool, Registry, Memory, Task, EventBus, ReasoningStrategy + ReAct loop + multi-agent orchestration), backends for Anthropic/OpenAI/Ollama/llama.cpp, a `.so` plugin system, CMake FetchContent packaging, and target audiences in ROS2 robotics, game dev, edge/embedded, and HFT.

**Problem with that roadmap:** it built a personal desktop assistant (voice → phone → ngrok → Oracle cloud) *first*, then extracted the library. That buries the actual product (the framework) under a kitchen-sink assistant. The goal is an **elite, top-1%, genuinely useful OSS contribution** — so the assistant should be *a* demo of the framework, not a prerequisite.

**This revision:** reorder to **framework-first**, demote the personal assistant to an optional post-1.0 showcase, and raise the engineering bar with three differentiators the original plan lacked.

## Decisions locked

- **Scope:** Framework-only first. Personal "Jarvis" assistant (voice/phone/cloud) → optional showcase **after** v1.0. Off the critical path.
- **Differentiators folded in:** (1) **MCP-native client**, (2) **native tool-calling + GBNF grammar-constrained JSON** for local models, (3) **async + streaming + cancellation**. (Tracing/eval not a headline phase — covered cheaply by the step-callback hook.)
- **Name:** decide later; `jarvis-cpp` is placeholder.

## Positioning (the reframe)

> **An in-process, offline-capable AI agent runtime for C++ — it runs inside latency-sensitive processes without blocking them (ROS2 nodes, game threads, drones, trading loops), where Python can't go, and speaks MCP so it works with the existing tool ecosystem on day one.**
>
> (Claim discipline: the defensible pitch is *embeddable + non-blocking* — async/cancel/streaming back it. Avoid bare "low-latency/HFT" claims: LLM latency dominates any runtime overhead, and that audience will nitpick.)

"LangChain for C++" stays as the one-line discovery hook, but the moat is **local-first + embeddable + MCP-native**, not API mimicry.

### Why the three differentiators matter
- **MCP-native** solves the ecosystem cold-start. Rather than waiting for the community to write `jarvis-gmail.so`, an MCP client lets jarvis-cpp use *thousands of existing MCP servers* (GitHub, Slack, filesystem, DBs) with zero custom code — instantly useful, wider audience, and process isolation sidesteps the plugin-ABI problem.
- **Native tool-calling + GBNF.** ReAct text-parsing is fragile/dated. Correctness = provider tool-use JSON schemas (Anthropic/OpenAI) for cloud, and **GBNF grammar-constrained decoding** to force valid tool-call JSON from local llama.cpp models.
- **Async + streaming + cancellation.** A blocking `agent.run()` would stall a ROS2 spin loop or a game frame. Need `runAsync()` (future + cancel token), token-streaming callbacks, timeouts.

### Engineering correction
The original `extern "C" Tool* create_tool()` plugin seam returns a C++ vtable object and passes `std::string`/`std::shared_ptr` across `.so` boundaries — UB across compiler/STL versions. **Fix:** MCP is the *primary* extension path (process-isolated); native in-process plugins, if kept, use a **pure C ABI**, documented as same-toolchain only. (Leaning MCP-only.)

## Architecture — core runtime (v0.x library)

1. **LLM backend** — `LLMClient` interface. Impls: `AnthropicClient`, `OpenAIClient`, `OllamaClient`, `LlamaCppClient` (behind CMake flag). Each supports chat completion, **native tool-calling** (send tool JSON schemas, receive structured tool calls), **streaming** (token callback), and for local backends **GBNF** grammar constraint. Factories: `anthropic()`, `openai()`, `ollama()`, `llamaCpp()`.
2. **Tool** — `name()`, `description()`, `inputSchema()` (JSON schema), and (contract v2, locked by the [2026-07-06 hardening spec](2026-07-06-phase0-hardening-design.md)) `execute(const std::string& args, const ToolContext& ctx) -> ToolResult`: the context carries a cooperative cancel token + deadline, the typed result distinguishes ok / retryable / fatal / timeout / cancelled so retries can branch on an enum, not string parsing. Rule: never throw; return an error result. `ToolRegistry`: thread-safe register/lookup + schema export. **One registry, three uniform sources** — all are `Tool` instances, treated identically by the agent:
   - **Built-in tools** ship with the library, phased by dependency + blast radius: **Calculator** and **guarded HttpRequest** in Phase 1; **jailed File I/O** in Phase 2–3; **guarded Shell** in Phase 4 (lands with the policy layer). **WebSearch is *not* an owned tool** — deferred to the MCP ecosystem (Phase 3) rather than shipping a provider integration + key management we'd have to babysit. Every dangerous tool carries a **`ToolGuard`** — a *per-tool* safety primitive (path jail / private-IP + scheme allowlist / timeout / output cap) that lives inside `execute()`. This is deliberately distinct from the **Phase-4 per-agent `ToolPolicy`/RBAC** layer: `ToolGuard` answers "is this call itself safe?", `ToolPolicy` answers "is *this agent* allowed this tool?". Decoupling them lets guarded HttpRequest (P1) and jailed File I/O (P2–3) ship long before multi-agent RBAC exists.
   - **User-defined C++ tools** — the primary in-process extension path. Two authoring styles: (a) subclass `Tool` for stateful/complex tools; (b) **`makeTool(name, desc, schema, lambda)`** `FunctionTool` adapter for the common case — zero subclassing, a `std::function<std::string(const json&)>` body. A small **schema helper** (`schema().str(...).num(...)`) generates `inputSchema()` so users never hand-write JSON schema.
   - **MCP tools** — adapted into `Tool` via `McpClient`.
   - Registration is **explicit** by default (`.withTool(...)` / `registry.registerTool(...)`) — predictable, no hidden control flow. An optional `JARVIS_REGISTER_TOOL(MyTool)` static self-registration macro is offered for power users, documented as advanced (global init-order caveat across the `dlopen` boundary).
3. **MCP** — `McpClient` (stdio + HTTP/SSE). Discovers a server's tools and adapts each into a `Tool`, uniform to the agent. **Trust model (decided with the 2026-07-06 hardening):** MCP tools register namespaced as `<server>__<tool>` (double underscore — provider tool-name charset is `[a-zA-Z0-9_-]`, max 64), so they can never collide with or shadow a built-in (the registry additionally throws on duplicate names by default). Tool *descriptions* from MCP servers are untrusted input injected into the prompt — cap their length, sanitize them, and document that MCP output must be treated as untrusted (tool-description injection / confused-deputy is the known attack class).
4. **Memory** — `Memory`: append, get-context, persist. Impls: `InMemoryMemory`, `SqliteMemory`. **Full design in [2026-07-04-memory-design.md](2026-07-04-memory-design.md)** — the short version: bounding = independent, composable, opt-in policies behind the unchanged 3-method seam (not a fixed tier hierarchy). Unbounded stays the default; `lastN` count-trim ships Phase 1 (growth cap, honestly documented as *not* a fit guarantee) plus an **always-on reactive overflow backstop** (truncate oversized message → trim harder → retry → clean error) since the first real 50-turn session against a paid API hits context limits; `maxTokens`/`autoWindow` token budgeting ships Phase 2 (pluggable `TokenCounter`: chars/4 estimator default, llama.cpp exact); `SummarizingMemory` is a P3+ opt-in decorator and the only impl permitted to call an LLM; long-term facts = `FactStore`, a separate retrieval interface (post-1.0), never on the `context()` path. Trimming always keeps the system message + in-flight exchange and treats an assistant-toolCalls turn + its id-paired tool results as one atomic unit.
5. **Reasoning strategy** — Strategy pattern: `ToolCalling` (native function-calling loop — default), `ReAct` (text fallback), `PlanAndExecute`. Template-method loop with hooks.
6. **Agent runtime** — `Agent` owns client/registry/memory/strategy/limits. API: `run(task)` (blocking convenience), `runAsync(task) -> std::future<Result>` with `CancelToken` + timeout + `maxIterations`, and a callback form (`onToken`, `onToolCall`, `onToolResult`, `onStep`) that doubles as the lightweight tracing hook. Fluent `AgentBuilder`. Retries with backoff; infinite-loop guard.
   - **Per-tool timeout (flagship-critical):** the loop must wrap each `execute()` in a timeout so one hung tool (slow HTTP, wedged Shell) can't freeze the whole agent thread. A blocking runtime that can stall is disqualifying for the ROS2/game targets that are the core pitch — this is the gap most on-brand to close. **Mechanism (decided): cooperative-first, watchdog as net.** C++ cannot force-stop a thread, so: (1) the loop stamps `ctx.deadline` + cancel token and every built-in tool honors them in its transport, guaranteeing first-party tools never hang; (2) a loop-level watchdog (`std::async` + `wait_for`) fires on expiry — signal cancel, grace ~250 ms, then record a `Timeout` observation and *abandon* the thread (documented: a non-cooperative tool leaks a parked thread until it returns); (3) `AgentCallbacks::onToolTimeout(name)` surfaces it to hosts. Genuinely untrusted code belongs behind MCP (Phase 3), which is process-isolated and killable.
   - **Cancellation granularity:** `CancelToken` is currently only checked at loop-top, so an in-flight HTTP call can't be aborted mid-request. Thread the token *into* the LLM/HTTP client (abort the socket) so cancel is sub-second, not "after this tool finishes."
   - **Parallel tool calls:** models emit multiple tool calls per turn; the loop runs them sequentially today. Independent calls should execute concurrently (thread pool) — folds into Phase-4 orchestration.
   - **Usage accounting:** `RunResult` surfaces `promptTokens`/`completionTokens`/`cost` from cloud backends (near-free once responses are parsed; feeds the Phase-6 benchmark numbers).
7. **Orchestration (multi-agent)** — `Orchestrator` routes tasks (Chain of Responsibility), `EventBus` (Observer) for agent-to-agent reactions, parallel execution via thread pool. *Later phase.*

**Cross-cutting:** config (keys via env), structured logging behind the step callback, semantic versioning, stable public API in `include/jarvis/jarvis.h`.

**Deps (light):** cpp-httplib (header-only), nlohmann/json, sqlite3; llama.cpp optional behind CMake flag. MIT license.

**Quality bar:** GitHub Actions CI Linux/macOS/Windows × gcc/clang/msvc; ASan/UBSan/TSan; clang-format + clang-tidy; doctest/Catch2 tests driven by a deterministic **MockLLM** (no API key); CMake FetchContent now, vcpkg/Conan later; doc site.

## Roadmap (framework-first). Each phase = its own spec → plan → build cycle.

- **Phase 0 — Foundations (Wk 1):** repo skeleton, CMake, CI, license, format/tidy, test harness + MockLLM, public header.
- **Phase 1 — Core single-agent + cloud (Wk 2–4):** `LLMClient` + Anthropic (+OpenAI); `ToolCalling` strategy + ReAct fallback; Tool/Registry + **`makeTool` lambda adapter + schema helper** (user-defined tool path); **first built-in tools: Calculator + guarded HttpRequest** (behind a mockable HTTP transport so tests stay offline); `ToolGuard` primitive; **per-tool timeout** in the loop; Memory (InMemory + Sqlite); `Agent`/`AgentBuilder`; `run` + `runAsync` + cancel (threaded into the client) + streaming; retries + loop guard (branching on `ToolResult` retryable/fatal — contract already in place); **usage/cost in `RunResult`**; **memory: `lastN` trim policy + always-on overflow backstop** (per the memory spec: turn-boundary rules, builder floor validation); per-call arg validation against the tool schema (required keys + primitive types, 64 KB cap) once nlohmann/json is in; tests. **Milestone: 12-line quickstart works against a real API.**
- **Phase 2 — Local-first (Wk 5–6):** Ollama + llama.cpp backends; **GBNF** tool-call JSON; **jailed File I/O tools** (`read_file`/`write_file`/`list_dir`, path-allowlisted, no `..` escape — needs only `ToolGuard`, not RBAC); **token-budget memory** — `TokenCounter` seam (estimator default, llama.cpp exact), `maxTokens` (clamped, output-reserve-aware, oversized-message truncation), `autoWindow`, `LLMClient::contextWindow()/tokenizer()` capability hooks; benchmark vs Python cold start; **RPi-5 fully-offline demo**.
- **Phase 3 — MCP-native (Wk 7–8):** `McpClient` (stdio + HTTP/SSE) + tool adaptation with the **`<server>__<tool>` namespacing + untrusted-description mitigations** (see MCP trust model above). Demo: agent using an off-the-shelf MCP server with **zero custom tool code**. **WebSearch arrives here via a public MCP server** (not an owned integration). **`SummarizingMemory`** opt-in decorator (P3+; may slip — not launch-blocking).
- **Phase 4 — Multi-agent orchestration (Wk 9–10):** Orchestrator + EventBus + routing + **parallel tool/agent exec** (thread pool); `PlanAndExecute`; **guarded Shell** built-in (ships with the policy layer); **tool access control** — a `ToolPolicy` / filtered-registry-view layer above the (still dumb) `ToolRegistry`, optionally tool **scopes** with execution-time denial + audit (e.g. coding agent gets GitHub, others don't, from a shared tool pool).
- **Phase 5 — Flagship demos + extension (Wk 11–12):** ROS2 planner node; game-NPC; optional native **C-ABI** plugin example; `CONTRIBUTING.md`; **`SECURITY.md` + vulnerability-disclosure policy** (table stakes for a project shipping shell/HTTP tools); 5 good-first-issues.
- **Phase 6 — Launch (Wk 13–15):** README (benchmarks, GIFs, comparison); CI badge; MIT; v1.0.0 criteria; Show HN → r/cpp → ROS Discourse → Unreal → r/raspberry_pi → llama.cpp discussions; dev.to blog.
- **Later / optional (post-1.0):** personal **Jarvis assistant** showcase — CLI → whisper.cpp voice → REST/phone → cloud; **`FactStore`** long-term memory (separate retrieval interface, `recall_facts`/`remember_fact` tools; embeddings/vector deps stay out of core permanently).

### Defaults
- **C++17 baseline** (compat with ROS2/Unreal/embedded). Async via `std::future` + thread pool; optional C++20 coroutine adapter later.
- **Two flagship demos committed:** RPi-5 offline + ROS2 planner. Game-NPC is a strong third, not a gate.

## Verification (per phase)
- **Unit/CI:** MockLLM-driven tests for agent loop, tool dispatch, memory windowing, retries/loop-guard; sanitizers clean; CI green on 3 OSes.
- **Phase 1:** 12-line quickstart against a real Anthropic key completes a multi-tool task; a deliberately-hung tool trips the per-tool timeout without hanging the agent; guarded HttpRequest rejects a private-IP/metadata URL; `RunResult` reports token usage; `lastN` trim never drops the system message nor orphans a `toolCallId`; a mocked `context_length_exceeded` triggers truncate→trim→retry then clean error — all provable offline via MockLLM + a mock HTTP transport.
- **Phase 2:** local model emits GBNF-valid tool JSON; jailed File I/O refuses a `..`-escape path; `maxTokens` clamps an over-window budget and reserves output headroom; capture RPi-5 offline run + cold-start/memory benchmarks.
- **Phase 3:** connect to a public MCP server, list tools, complete a task with no jarvis-specific tool code.
- **Phase 4:** orchestrator runs ≥2 agents in parallel; EventBus delivers cross-agent events; reproducible under MockLLM.
- **Phase 5/6:** each example builds on a fresh machine in <5 min; README benchmarks reproduce; CI badge green.

## Open items
- Final library name (parked).
- Native in-process plugins vs **MCP-only** extension (leaning MCP-only).
- Tool access control (RBAC): per-agent registries for now; `ToolPolicy`/scoped-access layer in Phase 4. **Distinct from `ToolGuard`** (per-tool safety, per above) — don't conflate the two.
- ~~Structured tool errors~~ **RESOLVED (2026-07-06 hardening):** typed `ToolResult` (ok/retryable/fatal/timeout/cancelled) + `ToolContext` (cancel, deadline) — implemented in Phase 0; retries/backoff branch on the enum in Phase 1.
