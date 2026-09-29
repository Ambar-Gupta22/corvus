#include <doctest/doctest.h>

#include <future>
#include <stdexcept>
#include <string>
#include <vector>

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

TEST_CASE("early stop returns the last assistant text, not a sentinel") {
    auto mock = std::make_shared<MockLLM>();
    mock->replyAndCallTool("partial answer", "echo", "{}");

    auto agent = AgentBuilder().withModel(mock).withTool(echoTool()).withMaxIterations(1).build();

    RunResult res = agent.run("go", {});

    CHECK_FALSE(res.completed);
    CHECK(res.output == "partial answer");
}

TEST_CASE("memory records the assistant tool-call turn and pairs results by id") {
    auto memory = inMemory();
    auto mock = std::make_shared<MockLLM>();
    mock->replyAndCallTool("thinking...", "echo", "{\"text\":\"hi\"}").reply("done");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(echoTool())
                     .withMemory(memory)
                     .withMaxIterations(5)
                     .build();
    agent.run("please echo hi", {});

    auto ctx = memory->context();
    // user -> assistant(with tool calls) -> tool(result) -> assistant(final)
    REQUIRE(ctx.size() == 4);

    CHECK(ctx[0].role == "user");

    CHECK(ctx[1].role == "assistant");
    CHECK(ctx[1].content == "thinking...");
    REQUIRE(ctx[1].toolCalls.size() == 1);
    CHECK(ctx[1].toolCalls[0].name == "echo");
    CHECK_FALSE(ctx[1].toolCalls[0].id.empty());

    CHECK(ctx[2].role == "tool");
    CHECK(ctx[2].name == "echo");
    CHECK(ctx[2].toolCallId == ctx[1].toolCalls[0].id);

    CHECK(ctx[3].role == "assistant");
    CHECK(ctx[3].content == "done");
}

TEST_CASE("assistant tool-call turn is recorded even when its text is empty") {
    auto memory = inMemory();
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("echo", "{}").reply("done");

    auto agent = AgentBuilder().withModel(mock).withTool(echoTool()).withMemory(memory).build();
    agent.run("go", {});

    auto ctx = memory->context();
    REQUIRE(ctx.size() == 4);
    CHECK(ctx[1].role == "assistant");
    CHECK(ctx[1].content.empty());
    REQUIRE(ctx[1].toolCalls.size() == 1);
    CHECK(ctx[2].toolCallId == ctx[1].toolCalls[0].id);
}

TEST_CASE("full-form tool receives a context and typed errors reach the model as ERROR text") {
    auto flaky = makeTool("flaky", "always busy", "{}",
                          [](const std::string&, const ToolContext& ctx) -> ToolResult {
                              CHECK_FALSE(ctx.expired());  // no deadline set in Phase 0
                              CHECK_FALSE(ctx.cancel.cancelled());
                              return ToolResult::retryable("upstream busy");
                          });

    auto mock = std::make_shared<MockLLM>();
    mock->callTool("flaky", "{}").reply("gave up");

    auto agent = AgentBuilder().withModel(mock).withTool(flaky).build();

    std::string observation;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observation = r; };

    RunResult res = agent.run("try the flaky tool", cb);

    CHECK(res.completed);
    CHECK(observation == "ERROR: upstream busy");
}

TEST_CASE("build() validates its inputs") {
    auto mock = std::make_shared<MockLLM>();
    mock->reply("ok");

    SUBCASE("no model") { CHECK_THROWS(AgentBuilder().build()); }
    SUBCASE("unimplemented strategy") {
        CHECK_THROWS_WITH(AgentBuilder().withModel(mock).withStrategy(Strategy::ReAct).build(),
                          doctest::Contains("only Strategy::ToolCalling is implemented"));
    }
    SUBCASE("non-positive maxIterations") {
        CHECK_THROWS(AgentBuilder().withModel(mock).withMaxIterations(0).build());
    }
}

TEST_CASE("build() twice on one builder yields independent agents") {
    auto mock = std::make_shared<MockLLM>();
    mock->reply("first").reply("second");

    AgentBuilder builder;
    builder.withModel(mock).withTool(echoTool());

    Agent a = builder.build();
    Agent b = builder.build();  // used to throw: tool re-registered into a's registry

    CHECK(a.run("task a").output == "first");
    CHECK(b.run("task b").output == "second");
}

TEST_CASE("default memory is not shared between agents from one builder") {
    // Observe memory through the model: MockLLM ignores input, so capture it.
    struct Spy : LLMClient {
        std::vector<std::size_t> seen;
        std::string name() const override { return "spy"; }
        LLMResponse complete(const std::vector<Message>& messages, const std::vector<ToolSpec>&,
                             const TokenCallback&) override {
            seen.push_back(messages.size());
            LLMResponse r;
            r.text = "ok";
            return r;
        }
    };
    auto spy = std::make_shared<Spy>();

    AgentBuilder builder;
    builder.withModel(spy);
    Agent a = builder.build();
    Agent b = builder.build();
    a.run("one");
    b.run("two");

    // Each agent's first turn sees only its own user message.
    REQUIRE(spy->seen.size() == 2);
    CHECK(spy->seen[0] == 1);
    CHECK(spy->seen[1] == 1);
}

TEST_CASE("repeated build() with a shared registry keeps anti-shadowing") {
    auto mock = std::make_shared<MockLLM>();
    auto registry = std::make_shared<ToolRegistry>();
    auto tool = echoTool();

    AgentBuilder builder;
    builder.withModel(mock).withRegistry(registry).withTool(tool);
    CHECK_NOTHROW(builder.build());
    CHECK_NOTHROW(builder.build());  // same tool object: idempotent
    CHECK(registry->size() == 1);

    // A different tool under the same name is still rejected.
    AgentBuilder other;
    other.withModel(mock).withRegistry(registry).withTool(echoTool());
    CHECK_THROWS_AS(other.build(), std::invalid_argument);
}

TEST_CASE("MockLLM tool-call ids stay unique across consume and re-enqueue") {
    MockLLM mock;
    mock.callTool("t", "{}");
    std::string first = mock.complete({}, {}).toolCalls.at(0).id;

    mock.callTool("t", "{}");  // queue is empty again here
    std::string second = mock.complete({}, {}).toolCalls.at(0).id;

    CHECK(first != second);
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

TEST_CASE("overlapping runs on one agent throw logic_error") {
    std::promise<void> entered;
    std::promise<void> release;
    auto releaseF = release.get_future().share();

    auto blocker = makeTool("block", "blocks until released", "{}",
                            [&](const std::string&) -> std::string {
                                entered.set_value();
                                releaseF.wait();
                                return "unblocked";
                            });

    auto mock = std::make_shared<MockLLM>();
    mock->callTool("block", "{}").reply("first-done");

    auto agent = AgentBuilder().withModel(mock).withTool(blocker).build();

    auto fut = agent.runAsync("first");
    entered.get_future().wait();  // first run is now mid-tool

    CHECK_THROWS_AS(agent.run("second"), std::logic_error);

    release.set_value();
    RunResult res = fut.get();
    CHECK(res.completed);
    CHECK(res.output == "first-done");
}

TEST_CASE("destroying the Agent mid-run is safe; the future completes") {
    std::promise<void> entered;
    std::promise<void> release;
    auto releaseF = release.get_future().share();

    auto blocker = makeTool("block", "blocks until released", "{}",
                            [&](const std::string&) -> std::string {
                                entered.set_value();
                                releaseF.wait();
                                return "unblocked";
                            });

    auto mock = std::make_shared<MockLLM>();
    mock->callTool("block", "{}").reply("survived");

    std::future<RunResult> fut;
    {
        auto agent = AgentBuilder().withModel(mock).withTool(blocker).build();
        fut = agent.runAsync("go");
        entered.get_future().wait();  // run is in flight
    }  // agent destroyed here

    release.set_value();
    RunResult res = fut.get();
    CHECK(res.completed);
    CHECK(res.output == "survived");
}

TEST_CASE("sequential runAsync calls on one agent both complete") {
    auto mock = std::make_shared<MockLLM>();
    mock->reply("one").reply("two");
    auto agent = AgentBuilder().withModel(mock).build();

    RunResult r1 = agent.runAsync("a").get();
    RunResult r2 = agent.runAsync("b").get();

    CHECK(r1.output == "one");
    CHECK(r2.output == "two");
}
