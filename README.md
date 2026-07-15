# corvus

[![CI](https://github.com/Ambar-Gupta22/corvus/actions/workflows/ci.yml/badge.svg)](https://github.com/Ambar-Gupta22/corvus/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
![Status](https://img.shields.io/badge/status-Phase%201%20·%20in%20progress-orange.svg)

**An in-process, offline-capable AI agent runtime for C++.**
It runs where Python can't — ROS2 nodes, game threads, drones, trading loops — and speaks [MCP](https://modelcontextprotocol.io) so it works with the existing tool ecosystem on day one.

> LangChain made building AI agents trivial in Python. corvus brings that to native C++17: native tool-calling, memory, an async agent loop with real cancellation, and local models via llama.cpp — zero Python runtime, one CMake target.

> ⚠️ **Status: early development (Phase 1 in progress).** The core agent loop, tool system, memory, offline test harness, and the mockable HTTP transport are merged. Real cloud clients (Anthropic/OpenAI) are landing now, PR by PR; llama.cpp and MCP follow. The `corvus` name is a working name. Watch the [roadmap](#roadmap).

---

## Quickstart

```cpp
#include <corvus/corvus.h>

int main() {
    using namespace corvus;

    auto agent = AgentBuilder()
        .withModel(anthropic("claude-haiku-4-5-20251001"))   // or openai(...), ollama(...)
        .withTool(makeTool("weather", "Get current weather",
                           schema().str("city", "city name"),
                           [](const std::string& args) { return fetchWeather(args); }))
        .withStrategy(Strategy::ToolCalling)
        .build();

    RunResult result = agent.run("What's the weather in Bangalore?");
    std::cout << result.output << "\n";
}
```

A user-defined tool is one `makeTool(...)` call — no subclassing, no hand-written JSON schema. Need non-blocking? `agent.runAsync(task, cancelToken)` returns a `std::future` and streams tokens through a callback, so a ROS2 spin loop or a game frame never stalls.

## Why corvus

C++ developers have no LangChain. The gap is real and the workarounds hurt: embedding Python (GIL, packaging, latency), shelling out to sidecar processes, or hand-rolling raw HTTP against provider APIs. corvus is a native runtime built around three commitments:

1. **MCP-native** — thousands of existing [MCP](https://modelcontextprotocol.io) servers become corvus tools with zero custom code. No ecosystem cold-start.
2. **Native tool-calling, not prompt hacks** — provider tool-use JSON for cloud models; grammar-constrained (GBNF) JSON for local llama.cpp models. ReAct exists as a fallback, not the default.
3. **Async + streaming + cancellation as first-class citizens** — `runAsync`, token callbacks, and a `CancelToken` that reaches all the way down to the socket. Built for processes that cannot block.

| | LangChain | llama.cpp | **corvus** |
|---|---|---|---|
| Language | Python | C/C++ | **C++17** |
| Embeddable in a C++ process | ❌ | ✅ | **✅** |
| Full agent loop (tools, memory, strategies) | ✅ | ❌ inference only | **✅** |
| Offline local models | partial | ✅ | **✅** |
| Non-blocking + cancellable runs | ❌ | n/a | **✅** |
| MCP tool ecosystem | partial | ❌ | **✅ (Phase 3)** |

## Architecture

Small, single-purpose units behind stable interfaces — every seam swappable, every seam mockable:

```
        AgentBuilder  ──build()──▶  Agent ──── run() / runAsync() + CancelToken
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                       ▼
          LLMClient               ToolRegistry              Memory
     (Anthropic, OpenAI,              │                (InMemory, Sqlite,
      Ollama, MockLLM)                │                 trimming policies)
              │            ┌──────────┼──────────┐
         HttpTransport     ▼          ▼          ▼
        (real / mock —  built-in   your C++     MCP tools
         tests offline)  tools    (makeTool)   (any server)
```

- **Tools never throw.** They return a typed `ToolResult` (ok / retryable / fatal / timeout / cancelled) — the loop branches on an enum, the model sees a clean `"ERROR: ..."` observation.
- **Every dangerous tool carries a `ToolGuard`** (path jail, private-IP block, output caps) inside its `execute()` — raw outbound HTTP from an LLM is an SSRF risk, and corvus treats it as one.
- **Public headers are a contract**: std types only, no third-party leakage, install/export that works with both `FetchContent` and `find_package`.

## Add to your project

```cmake
include(FetchContent)
FetchContent_Declare(corvus GIT_REPOSITORY https://github.com/Ambar-Gupta22/corvus GIT_TAG main)
FetchContent_MakeAvailable(corvus)
target_link_libraries(your_target PRIVATE corvus::corvus)
```

(Pin a release tag once v0.1 ships; until then `main` is kept always-green.)

## Build from source

Requires **CMake ≥ 3.18** and a **C++17** compiler (GCC ≥ 9, Clang ≥ 10, or MSVC 2019+). OpenSSL is optional — with it you get HTTPS; without it corvus still builds (local/offline backends don't need it).

```bash
cmake -S . -B build -DCORVUS_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Testing philosophy

The entire suite runs **offline, deterministic, key-free** — a scripted `MockLLM` plays the model and a scripted `MockHttpTransport` plays the network, so client wire formats are asserted byte-for-byte without a single socket. CI runs the matrix on Linux/macOS/Windows × gcc/clang/MSVC, plus AddressSanitizer, UBSanitizer, and ThreadSanitizer jobs. Every behavior change lands with a test.

## Roadmap

Framework-first; each phase is its own spec → plan → build cycle, landing as reviewed, CI-gated PRs. Full detail: [design spec](docs/specs/2026-06-29-jarvis-cpp-design.md).

- [x] **Phase 0 — foundations**: agent loop, tool system, memory, MockLLM, 3-OS CI + sanitizers ([hardened](docs/specs/2026-07-06-phase0-hardening-design.md))
- [ ] **Phase 1 — cloud backends** ← *in progress*
  - [x] Mockable HTTP transport seam ([PR #1](https://github.com/Ambar-Gupta22/corvus/pull/1), [explainer](docs/pr1-http-transport-explained.md))
  - [ ] `AnthropicClient` — native tool-calling, SSE streaming, typed errors
  - [ ] `OpenAIClient` (Chat Completions — also unlocks Ollama/vLLM/OpenRouter via `baseUrl`)
  - [ ] Retries + backoff · per-tool timeout · SqliteMemory · memory trimming · usage/cost · Calculator + guarded HttpRequest tools
- [ ] **Phase 2 — local-first**: Ollama + llama.cpp + GBNF, jailed file I/O, token-budget memory, Raspberry Pi demo
- [ ] **Phase 3 — MCP-native**: `McpClient` (stdio + HTTP/SSE), the MCP ecosystem as your toolbox
- [ ] **Phase 4 — multi-agent orchestration**: orchestrator, event bus, parallel execution, tool policy/RBAC
- [ ] **Phase 5 — flagship demos**: ROS2 planner node, game NPC
- [ ] **Phase 6 — launch**

## Documentation

| Doc | What it covers |
|---|---|
| [Design & roadmap spec](docs/specs/2026-06-29-jarvis-cpp-design.md) | Architecture, decisions, phases |
| [Phase 0 explained](docs/phase-0-explained.md) | Plain-language tour of the core runtime |
| [PR 1 explained](docs/pr1-http-transport-explained.md) | The HTTP transport seam — what, why, and what review caught |
| [Cloud clients design](docs/specs/2026-07-15-cloud-clients-design.md) | Phase 1 client subsystem spec |
| [Memory design](docs/specs/2026-07-04-memory-design.md) | Trimming policies, overflow backstop, phasing |

## Contributing

Early-stage and moving fast — issues and discussion welcome. `main` is always green: every PR builds and passes the full matrix before merge. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE).
