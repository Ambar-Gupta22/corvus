#include "corvus/agent_builder.h"

#include <stdexcept>

namespace corvus {

Agent AgentBuilder::build() {
    if (!llm_) {
        throw std::runtime_error("corvus: withModel() is required before build()");
    }
    if (strategy_ != Strategy::ToolCalling) {
        throw std::runtime_error(
            "corvus: only Strategy::ToolCalling is implemented; ReAct and PlanAndExecute are not "
            "available yet");
    }
    if (maxIterations_ < 1) {
        throw std::runtime_error("corvus: maxIterations must be >= 1");
    }
    // Defaults are locals, never written back to the builder: each build()
    // without an explicit memory/registry yields an independent agent, and a
    // second build() doesn't re-register into the first agent's registry.
    MemoryPtr memory = memory_ ? memory_ : inMemory();
    ToolRegistryPtr registry = registry_ ? registry_ : std::make_shared<ToolRegistry>();

    // Register any tools added via withTool(), whether or not an explicit
    // registry was supplied. Re-registering the identical tool object (a
    // repeated build() on a shared registry) is a no-op; a different tool
    // under the same name still throws (anti-shadowing).
    for (const auto& tool : tools_) {
        if (tool && registry->get(tool->name()) == tool) {
            continue;
        }
        registry->registerTool(tool);
    }

    return Agent(llm_, registry, memory, strategy_, maxIterations_);
}

}  // namespace corvus
