#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "corvus/tool.h"

namespace corvus {

// ToolRegistry — thread-safe home for every tool, regardless of source
// (built-in, user-defined C++, or MCP-adapted). The agent queries it to build
// the tool specs it hands to the model.
class ToolRegistry {
public:
    void registerTool(ToolPtr tool);

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
