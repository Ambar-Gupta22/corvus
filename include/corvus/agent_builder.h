#pragma once

#include <cstddef>
#include <utility>
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
        limits_.maxIterations = n;
        return *this;
    }

    // Loop limits (see AgentLimits). All have safe defaults; these tighten or
    // raise them.
    AgentBuilder& withMaxToolCallsPerTurn(int n) {
        limits_.maxToolCallsPerTurn = n;
        return *this;
    }
    AgentBuilder& withMaxToolCallsPerRun(int n) {
        limits_.maxToolCallsPerRun = n;
        return *this;
    }
    AgentBuilder& withMaxObservationBytes(std::size_t n) {
        limits_.maxObservationBytes = n;
        return *this;
    }

    // Optional tool gate (see ToolGate). A second call replaces the first.
    AgentBuilder& withToolGate(ToolGate gate) {
        gate_ = std::move(gate);
        return *this;
    }
    AgentBuilder& withToolGate(SimpleToolGate gate) {
        if (!gate) {
            gate_ = nullptr;
            return *this;
        }
        gate_ = [simple = std::move(gate)](const ToolCall& call, const CancelToken& /*cancel*/) {
            return simple(call);
        };
        return *this;
    }

    // Validates and assembles an Agent. Throws std::runtime_error if no model
    // was set, the strategy isn't ToolCalling, or any limit is < 1; throws
    // std::invalid_argument if a withTool() name collides with a different
    // tool already in the registry. Unset memory/registry get fresh defaults
    // per call, so building twice yields two independent agents.
    Agent build();

private:
    LLMClientPtr llm_;
    ToolRegistryPtr registry_;
    MemoryPtr memory_;
    std::vector<ToolPtr> tools_;
    Strategy strategy_ = Strategy::ToolCalling;
    AgentLimits limits_;
    ToolGate gate_;
};

}  // namespace corvus
