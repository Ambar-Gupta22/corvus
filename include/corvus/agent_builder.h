#pragma once

#include <vector>

#include "corvus/agent.h"

namespace corvus {

// AgentBuilder — fluent construction. Validates required fields at build()
// time and fails fast with a clear message rather than crashing inside run().
//
//   auto agent = corvus::AgentBuilder()
//       .withModel(corvus::anthropic("claude-haiku-4-5-20251001"))
//       .withTool(corvus::makeTool("calc", "...", schema, fn))
//       .build();
class AgentBuilder {
public:
    AgentBuilder& withModel(LLMClientPtr client) {
        llm_ = std::move(client);
        return *this;
    }
    AgentBuilder& withTool(ToolPtr tool) {
        tools_.push_back(std::move(tool));
        return *this;
    }
    AgentBuilder& withRegistry(ToolRegistryPtr registry) {
        registry_ = std::move(registry);
        return *this;
    }
    AgentBuilder& withMemory(MemoryPtr memory) {
        memory_ = std::move(memory);
        return *this;
    }
    AgentBuilder& withStrategy(Strategy strategy) {
        strategy_ = strategy;
        return *this;
    }
    AgentBuilder& withMaxIterations(int n) {
        maxIterations_ = n;
        return *this;
    }

    Agent build();

private:
    LLMClientPtr llm_;
    ToolRegistryPtr registry_;
    MemoryPtr memory_;
    std::vector<ToolPtr> tools_;
    Strategy strategy_ = Strategy::ToolCalling;
    int maxIterations_ = 10;
};

}  // namespace corvus
