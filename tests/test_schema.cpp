#include <doctest/doctest.h>

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

TEST_CASE("makeTool exposes name, description and schema") {
    auto t = makeTool("echo", "echoes text", schema().str("text", "the text"),
                      [](const std::string& a) -> std::string { return a; });
    CHECK(t->name() == "echo");
    CHECK(t->description() == "echoes text");
    CHECK(t->inputSchema().find("\"text\"") != std::string::npos);
}

TEST_CASE("FunctionTool enforces the never-throw contract") {
    auto t = makeTool("boom", "always throws", "{}",
                      [](const std::string&) -> std::string { throw std::runtime_error("kaboom"); });
    std::string r = t->execute("{}");
    CHECK(r.rfind("ERROR:", 0) == 0);
    CHECK(r.find("kaboom") != std::string::npos);
}
