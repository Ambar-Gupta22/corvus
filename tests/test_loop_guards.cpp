#include <doctest/doctest.h>

#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "corvus/agent_builder.h"
#include "corvus/mock_llm.h"
#include "corvus/tool.h"

using namespace corvus;

namespace {

// A tool that counts its executions and echoes a fixed payload.
ToolPtr countingTool(std::string name, std::shared_ptr<int> count, std::string payload = "ok") {
    return makeTool(std::move(name), "counts calls", "{}",
                    [count, payload](const std::string&) -> std::string {
                        ++*count;
                        return payload;
                    });
}

std::vector<std::pair<std::string, std::string>> calls(const std::string& tool, int n) {
    return std::vector<std::pair<std::string, std::string>>(static_cast<std::size_t>(n),
                                                            {tool, "{}"});
}

// Minimal UTF-8 well-formedness check (structure only; enough for cut points).
bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c >> 5) == 0x6) {
            len = 2;
        } else if ((c >> 4) == 0xE) {
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            len = 4;
        }
        if (len == 0 || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

// Runs one tool returning `payload` under `cap`; returns the observation.
std::string observe(const std::string& payload, std::size_t cap, RunResult* out = nullptr) {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("big", "{}").reply("done");
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("big", count, payload))
                     .withMaxObservationBytes(cap)
                     .build();
    std::string observation;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observation = r; };
    RunResult res = agent.run("go", cb);
    if (out != nullptr) *out = res;
    return observation;
}

}  // namespace

TEST_CASE("per-turn cap: extra calls are skipped but every call gets a paired result") {
    auto count = std::make_shared<int>(0);
    auto memory = inMemory();
    auto mock = std::make_shared<MockLLM>();
    mock->callTools(calls("t", 8)).reply("done");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withMemory(memory)
                     .withMaxToolCallsPerTurn(5)
                     .build();
    RunResult res = agent.run("go");

    CHECK(res.completed);
    CHECK(*count == 5);
    CHECK(res.toolCallsExecuted == 5);
    CHECK(res.toolCallsSkipped == 3);

    // user, assistant(8 calls), 8 tool results, final assistant
    auto ctx = memory->context();
    REQUIRE(ctx.size() == 11);
    REQUIRE(ctx[1].toolCalls.size() == 8);
    for (std::size_t k = 0; k < 8; ++k) {
        CHECK(ctx[2 + k].role == "tool");
        CHECK(ctx[2 + k].toolCallId == ctx[1].toolCalls[k].id);  // request order
    }
    CHECK(ctx[6].content == "ok");
    CHECK(ctx[7].content == "ERROR: skipped: per-turn tool call limit (5) reached");
    CHECK(ctx[9].content == "ERROR: skipped: per-turn tool call limit (5) reached");
}

TEST_CASE("default limits are on: 20 calls in one turn run only 16") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools(calls("t", 20)).reply("done");

    auto agent = AgentBuilder().withModel(mock).withTool(countingTool("t", count)).build();
    RunResult res = agent.run("go");

    CHECK(*count == 16);
    CHECK(res.toolCallsSkipped == 4);
}

TEST_CASE("per-run cap: the budget spans turns and the run still reaches its answer") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools(calls("t", 2)).callTools(calls("t", 2)).reply("final");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withMaxToolCallsPerRun(3)
                     .build();
    std::string last;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { last = r; };
    RunResult res = agent.run("go", cb);

    CHECK(res.completed);
    CHECK(res.output == "final");
    CHECK(*count == 3);
    CHECK(res.toolCallsSkipped == 1);
    CHECK(last == "ERROR: skipped: per-run tool call limit (3) reached");
}

TEST_CASE("unknown and denied calls count against the per-run budget") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools({{"nope", "{}"}, {"blocked", "{}"}, {"t", "{}"}}).reply("done");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withTool(countingTool("blocked", count))
                     .withToolGate([](const ToolCall& c) {
                         return c.name == "blocked" ? ToolDecision::deny("no")
                                                    : ToolDecision::allow();
                     })
                     .withMaxToolCallsPerRun(2)
                     .build();
    RunResult res = agent.run("go");

    CHECK(*count == 0);  // "t" was third, after the budget of 2 was spent
    CHECK(res.toolCallsDenied == 1);
    CHECK(res.toolCallsSkipped == 1);
    CHECK(res.toolCallsExecuted == 0);
}

TEST_CASE("tool gate: a denied call never executes and the model sees the reason") {
    auto moves = std::make_shared<int>(0);
    auto scans = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools({{"move_to", "{\"x\":40}"}, {"scan", "{}"}}).reply("done");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("move_to", moves))
                     .withTool(countingTool("scan", scans))
                     .withToolGate([](const ToolCall& c) {
                         if (c.name == "move_to") {
                             return ToolDecision::deny("target is outside the geofence");
                         }
                         return ToolDecision::allow();
                     })
                     .build();

    std::vector<std::string> observations;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observations.push_back(r); };
    RunResult res = agent.run("go", cb);

    CHECK(*moves == 0);
    CHECK(*scans == 1);
    CHECK(res.toolCallsDenied == 1);
    CHECK(res.toolCallsExecuted == 1);
    REQUIRE(observations.size() == 2);
    CHECK(observations[0] == "ERROR: denied: target is outside the geofence");
    CHECK(observations[1] == "ok");
}

TEST_CASE("tool gate: an empty deny reason renders as plain 'denied'") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("t", "{}").reply("done");
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withToolGate([](const ToolCall&) { return ToolDecision::deny(); })
                     .build();
    std::string observation;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observation = r; };
    agent.run("go", cb);
    CHECK(observation == "ERROR: denied");
}

TEST_CASE("tool gate fails closed and never leaks the exception text") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("t", "{}").reply("done");
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withToolGate([](const ToolCall&) -> ToolDecision {
                         throw std::runtime_error("db password=hunter2");
                     })
                     .build();
    std::string observation;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observation = r; };
    RunResult res = agent.run("go", cb);

    CHECK(*count == 0);
    CHECK(res.toolCallsDenied == 1);
    CHECK(observation == "ERROR: denied: tool gate failed");
    CHECK(observation.find("hunter2") == std::string::npos);
}

TEST_CASE("tool gate only sees calls that would otherwise run") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    // unknown tool, a real call, then one over the per-turn cap of 2
    mock->callTools({{"nope", "{}"}, {"t", "{}"}, {"t", "{}"}}).reply("done");

    std::vector<std::string> gated;
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withMaxToolCallsPerTurn(2)
                     .withToolGate([&](const ToolCall& c) {
                         gated.push_back(c.name);
                         return ToolDecision::allow();
                     })
                     .build();
    agent.run("go");

    REQUIRE(gated.size() == 1);
    CHECK(gated[0] == "t");
    CHECK(*count == 1);
}

TEST_CASE("a second withToolGate replaces the first") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("t", "{}").reply("done");
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withToolGate([](const ToolCall&) { return ToolDecision::deny("first"); })
                     .withToolGate([](const ToolCall&) { return ToolDecision::allow(); })
                     .build();
    agent.run("go");
    CHECK(*count == 1);
}

TEST_CASE("cancel-aware gate stops waiting when the run is cancelled") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("t", "{}").reply("never reached");

    std::promise<void> entered;
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withToolGate([&](const ToolCall&, const CancelToken& cancel) {
                         entered.set_value();
                         while (!cancel.cancelled()) {  // e.g. waiting on a human
                             std::this_thread::yield();
                         }
                         return ToolDecision::deny("cancelled while awaiting approval");
                     })
                     .build();

    CancelToken token;
    auto fut = agent.runAsync("go", token);
    entered.get_future().wait();
    token.cancel();
    RunResult res = fut.get();

    CHECK(*count == 0);
    CHECK_FALSE(res.completed);
    CHECK(res.output == "[cancelled]");
}

TEST_CASE("callbacks fire once per requested call, including skipped and denied ones") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools({{"t", "{}"}, {"blocked", "{}"}, {"t", "{}"}}).reply("done");

    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("t", count))
                     .withTool(countingTool("blocked", count))
                     .withMaxToolCallsPerTurn(2)
                     .withToolGate([](const ToolCall& c) {
                         return c.name == "blocked" ? ToolDecision::deny() : ToolDecision::allow();
                     })
                     .build();

    int requested = 0;
    std::vector<std::string> results;
    AgentCallbacks cb;
    cb.onToolCall = [&](const ToolCall&) { ++requested; };
    cb.onToolResult = [&](const std::string&, const std::string& r) { results.push_back(r); };
    agent.run("go", cb);

    CHECK(requested == 3);
    REQUIRE(results.size() == 3);
    CHECK(results[0] == "ok");
    CHECK(results[1] == "ERROR: denied");
    CHECK(results[2] == "ERROR: skipped: per-turn tool call limit (2) reached");
}

TEST_CASE("observation cap keeps head and tail with an exact byte-count marker") {
    const std::string original = "HEAD" + std::string(1000, 'x') + "TAIL";
    RunResult res;
    const std::string out = observe(original, 200, &res);

    CHECK(out.size() <= 200);
    CHECK(out.rfind("HEAD", 0) == 0);
    CHECK(out.substr(out.size() - 4) == "TAIL");
    CHECK(res.observationsTruncated == 1);

    const auto at = out.find("[...truncated ");
    REQUIRE(at != std::string::npos);
    const std::size_t removed = std::stoul(out.substr(at + 14));
    const std::string marker = "\n[...truncated " + std::to_string(removed) + " bytes...]\n";
    CHECK(out.find(marker) != std::string::npos);
    CHECK(out.size() - marker.size() + removed == original.size());  // count is exact
}

TEST_CASE("observation cap never splits a UTF-8 code point") {
    std::string euros;
    for (int k = 0; k < 400; ++k) euros += "\xE2\x82\xAC";  // U+20AC, 3 bytes each
    for (std::size_t cap = 60; cap < 70; ++cap) {  // varying cuts straddle code points
        const std::string out = observe(euros, cap);
        CHECK(out.size() <= cap);
        CHECK(validUtf8(out));
    }
}

TEST_CASE("observation cap smaller than the marker yields the marker alone") {
    const std::string out = observe(std::string(500, 'x'), 5);
    CHECK(out == "\n[...truncated 500 bytes...]\n");
}

TEST_CASE("an observation under the cap is unchanged and not counted") {
    RunResult res;
    CHECK(observe("short result", 200, &res) == "short result");
    CHECK(res.observationsTruncated == 0);
}

TEST_CASE("the capped observation is what memory stores") {
    auto count = std::make_shared<int>(0);
    auto memory = inMemory();
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("big", "{}").reply("done");
    auto agent = AgentBuilder()
                     .withModel(mock)
                     .withTool(countingTool("big", count, std::string(5000, 'y')))
                     .withMemory(memory)
                     .withMaxObservationBytes(100)
                     .build();
    agent.run("go");
    auto ctx = memory->context();
    REQUIRE(ctx.size() == 4);
    CHECK(ctx[2].content.size() <= 100);
    CHECK(ctx[2].content.find("[...truncated ") != std::string::npos);
}

TEST_CASE("build() rejects limits below 1") {
    auto mock = std::make_shared<MockLLM>();
    CHECK_THROWS_WITH(AgentBuilder().withModel(mock).withMaxToolCallsPerTurn(0).build(),
                      doctest::Contains("maxToolCallsPerTurn"));
    CHECK_THROWS_WITH(AgentBuilder().withModel(mock).withMaxToolCallsPerRun(0).build(),
                      doctest::Contains("maxToolCallsPerRun"));
    CHECK_THROWS_WITH(AgentBuilder().withModel(mock).withMaxObservationBytes(0).build(),
                      doctest::Contains("maxObservationBytes"));
}

TEST_CASE("the original 5-argument Agent constructor still works with default limits") {
    auto count = std::make_shared<int>(0);
    auto mock = std::make_shared<MockLLM>();
    mock->callTools(calls("t", 20)).reply("done");
    auto registry = std::make_shared<ToolRegistry>();
    registry->registerTool(countingTool("t", count));

    Agent agent(mock, registry, inMemory(), Strategy::ToolCalling, 5);
    RunResult res = agent.run("go");

    CHECK(res.completed);
    CHECK(*count == 16);
    CHECK(res.toolCallsSkipped == 4);
}
