#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "corvus/tool.h"

namespace corvus {

// What registerTool() does when a tool with the same name already exists.
// Default is Error: silently replacing a tool by name is how a malicious or
// misconfigured tool shadows a trusted one, so replacement must be explicit.
enum class OverwritePolicy { Error, Replace };

// ToolRegistry — thread-safe home for every tool, regardless of source
// (built-in, user-defined C++, or MCP-adapted). The agent queries it to build
// the tool specs it hands to the model.
class ToolRegistry {
public:
    void registerTool(ToolPtr tool, OverwritePolicy policy = OverwritePolicy::Error);

    ToolPtr get(const std::string& name) const;
    bool has(const std::string& name) const;
    std::vector<ToolPtr> all() const;
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, ToolPtr> tools_;
};

using ToolRegistryPtr = std::shared_ptr<ToolRegistry>;

}  // namespace corvus
