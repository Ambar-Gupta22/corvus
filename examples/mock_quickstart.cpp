// mock_quickstart — the shape of the corvus API, runnable offline.
//
// Swap `mock` for `corvus::anthropic("claude-haiku-4-5-20251001")` (Phase 1)
// and this becomes a real agent. The builder/tool/run code stays identical.
#include <iostream>

#include "corvus/corvus.h"

// The tool's arguments, as a plain struct. corvus derives the JSON Schema the
// model sees from the field() list below and parses each call into this.
struct CalcArgs {
    std::string expression;
};

int main() {
    using namespace corvus;

    // A user-defined tool in one expression — no subclassing, no JSON parsing.
    auto calculator =
        typedTool<CalcArgs>("calculator", "Evaluates a simple arithmetic expression.")
            .field("expression", &CalcArgs::expression, "e.g. '2 + 2'")
            .run([](const CalcArgs& a) -> std::string {
                // A real tool would evaluate a.expression. Kept trivial here.
                return a.expression == "2 + 2" ? "4" : "unsupported";
            });

    // Deterministic backend so the example runs without a key.
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("calculator", "{\"expression\":\"2 + 2\"}").reply("2 + 2 = 4");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(calculator)
                     .withStrategy(Strategy::ToolCalling)
                     .build();

    AgentCallbacks cb;
    cb.onToolCall = [](const ToolCall& c) {
        std::cout << "  -> calling " << c.name << " " << c.arguments << "\n";
    };

    RunResult result = agent.run("What is 2 + 2?", cb);
    std::cout << "Answer: " << result.output << "\n";
    return 0;
}
