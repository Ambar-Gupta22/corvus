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

Three rules:
1. **Never throw from a tool.** In the simple form above, a thrown exception is caught and becomes a fatal error. To report failures deliberately, use the full form, which returns a typed `corvus::ToolResult` — `ToolResult::ok(text)`, `ToolResult::retryable(why)`, or `ToolResult::fatal(why)`; the model sees failures as `"ERROR: <why>"`. `makeTool` enforces never-throw for lambdas; `Tool` subclasses must catch everything themselves.
2. **Honor cancellation if your tool can block.** The full form receives a `corvus::ToolContext`; check `ctx.cancel.cancelled()` and `ctx.expired()` in long-running work. C++ can't stop a thread for you.
3. **Write a good `description`.** The model decides whether to call your tool based entirely on it.

```cpp
auto lookup = corvus::makeTool(
    "lookup", "Look up a record by id.", corvus::schema().str("id", "record id"),
    [](const std::string& args, const corvus::ToolContext& ctx) -> corvus::ToolResult {
        if (ctx.cancel.cancelled()) return {corvus::ToolResult::Status::Cancelled, ""};
        // ... on a transient failure: return corvus::ToolResult::retryable("db busy");
        return corvus::ToolResult::ok("record: ...");
    });
```

For stateful or complex tools, subclass `corvus::Tool` directly (`name` / `description` / `inputSchema` / `execute(args, ctx)`).

New to the codebase? The [docs index](docs/README.md) has a guided reading path.

## Code style

- C++17. Formatting via `.clang-format` (run `clang-format -i`), linting via `.clang-tidy`.
- Keep public headers in `include/corvus/` dependency-light and stable — they are a contract.
- Match the surrounding style; keep each unit small and single-purpose.

## Pull requests

- One focused change per PR, on a `feat/`, `fix/`, `docs/`, or `ci/` branch. Include tests. Make sure `ctest` passes locally.
- CI runs build + tests on Linux/macOS/Windows plus ASan/UBSan and TSan jobs; keep it green. PRs are squash-merged.
- **Keep the docs current:** a PR that changes a public header updates that header's comments **and** its entry in [docs/CODE_TOUR.md](docs/CODE_TOUR.md); a merged feature PR adds an entry to [docs/HISTORY.md](docs/HISTORY.md).
