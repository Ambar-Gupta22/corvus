# corvus

**An in-process, offline-capable AI agent runtime for C++.**
It runs where Python can't — ROS2 nodes, game threads, drones, trading loops — and speaks [MCP](https://modelcontextprotocol.io) so it works with the existing tool ecosystem.

> LangChain made building AI agents trivial in Python. corvus brings that to native C++: tool-calling, memory, an async agent loop, and local models via llama.cpp — with zero Python runtime.

> ⚠️ **Status: early development (v0.0.1, Phase 0).** Core agent loop + tool system + offline test harness are in place. Real LLM backends (Anthropic/OpenAI/Ollama), llama.cpp, and MCP land in upcoming phases — see the [roadmap](docs/specs/2026-06-29-jarvis-cpp-design.md). The `corvus` name is a working name.

## Quickstart

```cpp
#include <corvus/corvus.h>

int main() {
    using namespace corvus;

    auto agent = AgentBuilder()
        .withModel(anthropic("claude-haiku-4-5-20251001"))   // or ollama(...), llamaCpp(...)
        .withTool(makeTool("weather", "Get current weather",
                           schema().str("city", "city name"),
                           [](const std::string& args) { return fetchWeather(args); }))
        .withStrategy(Strategy::ToolCalling)
        .build();

    RunResult result = agent.run("What's the weather in Bangalore?");
    std::cout << result.output << "\n";
}
```

A user-defined tool is one `makeTool(...)` call — no subclassing, no hand-written JSON schema. Built-in tools, your C++ tools, and MCP tools all share one registry and are treated identically by the agent.

## Three tool sources, one registry

```
                     ToolRegistry
                          |
      +-------------------+-------------------+
      |                   |                   |
  Built-in tools    User C++ tools        MCP tools
  (web, calc, ...)  (makeTool / Tool)   (any MCP server)
```

## Add to your project (4 lines)

```cmake
include(FetchContent)
FetchContent_Declare(corvus GIT_REPOSITORY https://github.com/<you>/corvus GIT_TAG v0.0.1)
FetchContent_MakeAvailable(corvus)
target_link_libraries(your_target PRIVATE corvus::corvus)
```

## Build from source

Requires **CMake ≥ 3.16** and a **C++17** compiler (GCC ≥ 9, Clang ≥ 10, or MSVC 2019+).

```bash
cmake -S . -B build -DCORVUS_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite uses a deterministic `MockLLM`, so it runs fully offline with no API key.

## Why corvus

| Framework  | Language | Embeddable in C++ | Offline local models |
|------------|----------|-------------------|----------------------|
| LangChain  | Python   | ❌                | partial              |
| llama.cpp  | C/C++    | ✅                | ✅ (inference only)  |
| **corvus** | **C++17**| **✅**            | **✅ (full agent)**  |

## Roadmap

Framework-first. Each phase is its own spec → plan → build cycle. Full detail: [docs/specs/2026-06-29-jarvis-cpp-design.md](docs/specs/2026-06-29-jarvis-cpp-design.md).

- **Phase 0** — foundations: core agent loop, tool system, memory, MockLLM, CI ← *you are here*
- **Phase 1** — real cloud backends (Anthropic/OpenAI) + native tool calling + streaming
- **Phase 2** — local-first: Ollama + llama.cpp + GBNF, Raspberry Pi offline demo
- **Phase 3** — MCP-native client
- **Phase 4** — multi-agent orchestration
- **Phase 5** — flagship demos (ROS2, game NPC)
- **Phase 6** — launch

## License

MIT — see [LICENSE](LICENSE).
