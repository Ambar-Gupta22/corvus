#include <doctest/doctest.h>

#include "corvus/agent_builder.h"
#include "corvus/mock_llm.h"
#include "corvus/schema.h"
#include "corvus/tool.h"

using namespace corvus;

namespace {
ToolPtr echoTool() {
    return makeTool("echo", "echoes the args", schema().str("text", "text"),
                    [](const std::string& a) -> std::string { return "echoed:" + a; });
}
}  // namespace

TEST_CASE("agent runs a tool call then returns the final answer") {
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("echo", "{\"text\":\"hi\"}").reply("done");

    auto agent = AgentBuilder().withModel(mock).withTool(echoTool()).withMaxIterations(5).build();

    int toolResults = 0;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string&) { ++toolResults; };

    RunResult res = agent.run("please echo hi", cb);

    CHECK(res.completed);
    CHECK(res.output == "done");
    CHECK(res.iterations == 2);
    CHECK(toolResults == 1);
}

TEST_CASE("unknown tool yields an error observation but the loop continues") {
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("nope", "{}").reply("recovered");

    auto agent = AgentBuilder().withModel(mock).withMaxIterations(5).build();

    std::string lastObservation;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { lastObservation = r; };

    RunResult res = agent.run("call a missing tool", cb);

    CHECK(res.completed);
    CHECK(res.output == "recovered");
    CHECK(lastObservation.rfind("ERROR: unknown tool", 0) == 0);
}

TEST_CASE("max-iterations guard stops a runaway loop") {
    auto mock = std::make_shared<MockLLM>();
    // Queue more tool calls than the iteration budget.
    mock->callTool("echo", "{}").callTool("echo", "{}").callTool("echo", "{}");

    auto agent = AgentBuilder().withModel(mock).withTool(echoTool()).withMaxIterations(2).build();

    RunResult res = agent.run("loop forever", {});

    CHECK_FALSE(res.completed);
    CHECK(res.iterations == 2);
}

TEST_CASE("build() throws when no model is set") {
    CHECK_THROWS(AgentBuilder().build());
}

TEST_CASE("runAsync returns a future and honors a pre-cancelled token") {
    auto mock = std::make_shared<MockLLM>();
    mock->reply("async-done");
    auto agent = AgentBuilder().withModel(mock).build();

    SUBCASE("normal async completion") {
        auto fut = agent.runAsync("go");
        RunResult res = fut.get();
        CHECK(res.completed);
        CHECK(res.output == "async-done");
    }

    SUBCASE("cancelled before any iteration") {
        CancelToken token;
        token.cancel();
        auto fut = agent.runAsync("go", token);
        RunResult res = fut.get();
        CHECK_FALSE(res.completed);
        CHECK(res.output == "[cancelled]");
        CHECK(res.iterations == 0);
    }
}
