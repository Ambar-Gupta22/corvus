#include "corvus/agent_builder.h"

#include <stdexcept>

namespace corvus {

Agent AgentBuilder::build() {
    if (!llm_) {
        throw std::runtime_error("corvus: withModel() is required before build()");
    }
    if (!memory_) {
        memory_ = inMemory();  // sensible default
    }
    if (!registry_) {
        registry_ = std::make_shared<ToolRegistry>();
    }
    // Register any tools added via withTool(), whether or not an explicit
    // registry was supplied.
    for (auto& tool : tools_) {
        registry_->registerTool(tool);
    }

    return Agent(llm_, registry_, memory_, strategy_, maxIterations_);
}

}  // namespace corvus
