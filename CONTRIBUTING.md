# Contributing to corvus

Thanks for your interest. corvus is in early development — issues, ideas, and PRs are all welcome.

## Build & test

Requires CMake ≥ 3.18 and a C++17 compiler (GCC ≥ 9, Clang ≥ 10, MSVC 2019+).

```bash
cmake -S . -B build -DCORVUS_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests use a deterministic `MockLLM` — no API key, no network. Add a test for every behavior change.

## Add a tool in 5 minutes

The common case needs no class — just `makeTool`:

```cpp
#include <corvus/corvus.h>

auto myTool = corvus::makeTool(
    "stock_price",                                   // name the model calls
    "Get the latest price for a stock ticker.",      // the model reads this to decide when to call
    corvus::schema().str("ticker", "e.g. AAPL"),     // JSON schema, generated for you
    [](const std::string& args) -> std::string {     // body: args is a JSON object string
        // ... fetch and return the result as text ...
        return "AAPL: $192.30";
    });

agentBuilder.withTool(myTool);
```

Two rules:
1. **Never throw from a tool.** Return a string starting with `ERROR: ` on failure — the agent reads it and decides whether to retry. (`makeTool` wraps your lambda to enforce this, but native `Tool` subclasses must follow it manually.)
2. **Write a good `description`.** The model decides whether to call your tool based entirely on it.

For stateful or complex tools, subclass `corvus::Tool` directly (`name` / `description` / `inputSchema` / `execute`).

## Code style

- C++17. Formatting via `.clang-format` (run `clang-format -i`), linting via `.clang-tidy`.
- Keep public headers in `include/corvus/` dependency-light and stable — they are a contract.
- Match the surrounding style; keep each unit small and single-purpose.

## Pull requests

- One focused change per PR. Include tests. Make sure `ctest` passes locally.
- CI runs build + tests on Linux/macOS/Windows and a sanitizer pass; keep it green.
