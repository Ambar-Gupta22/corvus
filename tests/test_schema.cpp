#include <doctest/doctest.h>

#include <stdexcept>

#include "corvus/schema.h"
#include "corvus/tool.h"

using namespace corvus;

TEST_CASE("schema builds a JSON Schema object with a required array") {
    std::string s = schema().str("city", "city name").integer("days", "forecast length", false);

    CHECK(s.find("\"type\":\"object\"") != std::string::npos);
    CHECK(s.find("\"city\":{\"type\":\"string\"") != std::string::npos);
    CHECK(s.find("\"days\":{\"type\":\"integer\"") != std::string::npos);
    // Only required fields appear in the required array.
    CHECK(s.find("\"required\":[\"city\"]") != std::string::npos);
}

TEST_CASE("schema escapes quotes in descriptions") {
    std::string s = schema().str("q", "use \"quotes\" here");
    CHECK(s.find("\\\"quotes\\\"") != std::string::npos);
}

TEST_CASE("schema escapes control characters so the JSON stays valid") {
    // Adjacent literals keep the hex escape from swallowing the next char.
    std::string s = schema().str("q", "bad\x01" "char");
    CHECK(s.find("\\u0001") != std::string::npos);
    CHECK(s.find('\x01') == std::string::npos);
}

TEST_CASE("makeTool exposes name, description and schema") {
    auto t = makeTool("echo", "echoes text", schema().str("text", "the text"),
                      [](const std::string& a) -> std::string { return a; });
    CHECK(t->name() == "echo");
    CHECK(t->description() == "echoes text");
    CHECK(t->inputSchema().find("\"text\"") != std::string::npos);
}

TEST_CASE("simple-form makeTool wraps the return value in an Ok result") {
    auto t = makeTool("echo", "echoes text", "{}",
                      [](const std::string& a) -> std::string { return a; });
    ToolResult r = t->execute("payload", ToolContext{});
    CHECK(r.status == ToolResult::Status::Ok);
    CHECK(r.content == "payload");
}

TEST_CASE("FunctionTool enforces the never-throw contract") {
    auto t = makeTool("boom", "always throws", "{}",
                      [](const std::string&) -> std::string { throw std::runtime_error("kaboom"); });
    ToolResult r = t->execute("{}", ToolContext{});
    CHECK(r.status == ToolResult::Status::FatalError);
    CHECK(r.content.find("kaboom") != std::string::npos);
}

TEST_CASE("full-form makeTool passes the context through") {
    auto t = makeTool("ctx", "context-aware", "{}",
                      [](const std::string&, const ToolContext& ctx) -> ToolResult {
                          return ctx.cancel.cancelled() ? ToolResult{ToolResult::Status::Cancelled, ""}
                                                        : ToolResult::ok("not cancelled");
                      });

    ToolContext ctx;
    ToolResult r1 = t->execute("{}", ctx);
    CHECK(r1.status == ToolResult::Status::Ok);

    ctx.cancel.cancel();
    ToolResult r2 = t->execute("{}", ctx);
    CHECK(r2.status == ToolResult::Status::Cancelled);
}
