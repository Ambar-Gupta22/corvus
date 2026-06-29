#include "corvus/tool_registry.h"

#include <stdexcept>

namespace corvus {

void ToolRegistry::registerTool(ToolPtr tool) {
    if (!tool) {
        throw std::invalid_argument("corvus: registerTool() received a null tool");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    tools_[tool->name()] = std::move(tool);
}

ToolPtr ToolRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    return it == tools_.end() ? nullptr : it->second;
}

bool ToolRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.find(name) != tools_.end();
}

std::vector<ToolPtr> ToolRegistry::all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolPtr> out;
    out.reserve(tools_.size());
    for (const auto& kv : tools_) {
        out.push_back(kv.second);
    }
    return out;
}

std::size_t ToolRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.size();
}

}  // namespace corvus
