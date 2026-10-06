#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "corvus/agent_builder.h"
#include "corvus/mock_llm.h"
#include "corvus/schema.h"
#include "corvus/typed_tool.h"

using namespace corvus;

namespace {

struct MoveArgs {
    double x = 0;
    double y = 0;
    std::string frame = "map";
};

ToolPtr moveTool() {
    return typedTool<MoveArgs>("move_to", "Move the robot")
        .field("x", &MoveArgs::x, "target x")
        .field("y", &MoveArgs::y, "target y")
        .field("frame", &MoveArgs::frame, "TF frame", Optional)
        .run([](const MoveArgs& a) {
            return "moved to " + std::to_string(static_cast<int>(a.x)) + "," +
                   std::to_string(static_cast<int>(a.y)) + " in " + a.frame;
        });
}

}  // namespace

TEST_CASE("typedTool schema equals the equivalent hand-written schema") {
    struct All {
        std::string s;
        double n = 0;
        int i = 0;
        bool b = false;
        std::optional<std::string> opt;
    };
    auto tool = typedTool<All>("all", "every kind")
                    .field("s", &All::s, "a string")
                    .field("n", &All::n, "a number")
                    .field("i", &All::i, "an integer")
                    .field("b", &All::b, "a flag", Optional)
                    .field("opt", &All::opt, "maybe")
                    .run([](const All&) { return std::string("ok"); });

    std::string expected = schema()
                               .str("s", "a string")
                               .num("n", "a number")
                               .integer("i", "an integer")
                               .boolean("b", "a flag", false)
                               .str("opt", "maybe", false);
    CHECK(tool->inputSchema() == expected);
    CHECK(tool->name() == "all");
    CHECK(tool->description() == "every kind");
}

TEST_CASE("typedTool fills the struct and returns the simple-form output") {
    ToolResult r = moveTool()->execute(R"({"x":1,"y":2,"frame":"odom"})", ToolContext{});
    CHECK(r.status == ToolResult::Status::Ok);
    CHECK(r.content == "moved to 1,2 in odom");
}

TEST_CASE("typedTool keeps the member default for an absent optional field") {
    ToolResult r = moveTool()->execute(R"({"x":3,"y":4,"extra":"ignored"})", ToolContext{});
    CHECK(r.status == ToolResult::Status::Ok);
    CHECK(r.content == "moved to 3,4 in map");
}

TEST_CASE("typedTool reports missing and mistyped fields as retryable") {
    auto tool = moveTool();

    ToolResult missing = tool->execute(R"({"x":1})", ToolContext{});
    CHECK(missing.status == ToolResult::Status::RetryableError);
    CHECK(missing.content == "invalid arguments: missing required field 'y'");

    ToolResult mistyped = tool->execute(R"({"x":"one","y":2})", ToolContext{});
    CHECK(mistyped.status == ToolResult::Status::RetryableError);
    CHECK(mistyped.content == "invalid arguments: field 'x' must be a number");

    ToolResult badJson = tool->execute("{oops", ToolContext{});
    CHECK(badJson.status == ToolResult::Status::RetryableError);
    CHECK(badJson.content == "invalid arguments: arguments are not valid JSON");
}

TEST_CASE("typedTool std::optional members are optional and nullopt when absent") {
    struct Q {
        std::optional<std::int64_t> limit;
    };
    auto tool = typedTool<Q>("q", "query")
                    .field("limit", &Q::limit, "max rows")
                    .run([](const Q& q) {
                        return q.limit ? std::to_string(*q.limit) : std::string("none");
                    });
    CHECK(tool->execute("{}", ToolContext{}).content == "none");
    CHECK(tool->execute(R"({"limit":null})", ToolContext{}).content == "none");
    CHECK(tool->execute(R"({"limit":25})", ToolContext{}).content == "25");
    CHECK(tool->inputSchema().find("\"required\":[]") != std::string::npos);
}

TEST_CASE("typedTool range-checks integers into the member type") {
    struct Small {
        std::int8_t v = 0;
        unsigned u = 0;
    };
    auto tool = typedTool<Small>("small", "narrow ints")
                    .field("v", &Small::v, "int8")
                    .field("u", &Small::u, "unsigned", Optional)
                    .run([](const Small& s) { return std::to_string(s.v); });

    CHECK(tool->execute(R"({"v":-128})", ToolContext{}).content == "-128");

    ToolResult over = tool->execute(R"({"v":200})", ToolContext{});
    CHECK(over.status == ToolResult::Status::RetryableError);
    CHECK(over.content == "invalid arguments: field 'v' must be an integer in [-128, 127]");

    CHECK(tool->execute(R"({"v":1,"u":-1})", ToolContext{}).status ==
          ToolResult::Status::RetryableError);
    CHECK(tool->execute(R"({"v":1.5})", ToolContext{}).status ==
          ToolResult::Status::RetryableError);
}

TEST_CASE("typedTool full form receives the context and returns its own status") {
    struct Empty {};
    CancelToken cancel;
    auto tool = typedTool<Empty>("ctx", "uses context")
                    .run([](const Empty&, const ToolContext& ctx) {
                        return ctx.cancel.cancelled() ? ToolResult::retryable("busy")
                                                      : ToolResult::ok("idle");
                    });
    ToolContext ctx;
    ctx.cancel = cancel;
    CHECK(tool->execute("", ctx).content == "idle");
    cancel.cancel();
    ToolResult r = tool->execute("", ctx);
    CHECK(r.status == ToolResult::Status::RetryableError);
    CHECK(r.content == "busy");
}

TEST_CASE("typedTool rejects empty and duplicate field keys at build time") {
    CHECK_THROWS_AS(typedTool<MoveArgs>("t", "d").field("", &MoveArgs::x, "x"),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        typedTool<MoveArgs>("t", "d").field("x", &MoveArgs::x, "x").field("x", &MoveArgs::y, "y"),
        std::invalid_argument);
}

TEST_CASE("typedTool turns an exception from the body into a fatal result") {
    struct Empty {};
    auto tool = typedTool<Empty>("boom", "throws").run([](const Empty&) -> std::string {
        throw std::runtime_error("kaboom");
    });
    ToolResult r = tool->execute("{}", ToolContext{});
    CHECK(r.status == ToolResult::Status::FatalError);
    CHECK(r.content == "kaboom");
}

TEST_CASE("typedTool works end-to-end through the agent loop") {
    auto mock = std::make_shared<MockLLM>();
    mock->callTool("move_to", R"({"x":1,"y":2})")
        .callTool("move_to", R"({"x":1})")
        .reply("done");

    auto agent = AgentBuilder().withModel(mock).withTool(moveTool()).withMaxIterations(5).build();

    std::vector<std::string> observations;
    AgentCallbacks cb;
    cb.onToolResult = [&](const std::string&, const std::string& r) { observations.push_back(r); };

    RunResult res = agent.run("move", cb);
    CHECK(res.completed);
    REQUIRE(observations.size() == 2);
    CHECK(observations[0] == "moved to 1,2 in map");
    CHECK(observations[1] == "ERROR: invalid arguments: missing required field 'y'");
}
