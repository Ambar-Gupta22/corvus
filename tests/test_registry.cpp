#include <doctest/doctest.h>

#include "corvus/tool.h"
#include "corvus/tool_registry.h"

using namespace corvus;

namespace {
ToolPtr trivial(const std::string& name, const std::string& desc = "desc") {
    return makeTool(name, desc, "{}", [](const std::string&) -> std::string { return "ok"; });
}
}  // namespace

TEST_CASE("registry registers, finds, and lists tools") {
    ToolRegistry reg;
    CHECK(reg.size() == 0);

    reg.registerTool(trivial("a"));
    reg.registerTool(trivial("b"));

    CHECK(reg.size() == 2);
    CHECK(reg.has("a"));
    CHECK(reg.has("b"));
    CHECK_FALSE(reg.has("missing"));
    // Extra parens: keep doctest from trying to stringify a shared_ptr<Tool>.
    CHECK((reg.get("a") != nullptr));
    CHECK((reg.get("missing") == nullptr));
    CHECK(reg.all().size() == 2);
}

TEST_CASE("registering a duplicate name throws by default (no silent shadowing)") {
    ToolRegistry reg;
    reg.registerTool(trivial("dup"));
    CHECK_THROWS(reg.registerTool(trivial("dup")));
    CHECK(reg.size() == 1);
}

TEST_CASE("OverwritePolicy::Replace replaces deliberately") {
    ToolRegistry reg;
    reg.registerTool(trivial("dup", "old"));
    reg.registerTool(trivial("dup", "new"), OverwritePolicy::Replace);

    CHECK(reg.size() == 1);
    CHECK(reg.get("dup")->description() == "new");
}

TEST_CASE("registering a null tool throws") {
    ToolRegistry reg;
    CHECK_THROWS(reg.registerTool(nullptr));
}
