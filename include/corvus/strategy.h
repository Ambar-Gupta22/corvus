#pragma once

namespace corvus {

// Reasoning strategy — how the agent drives the model.
//   ToolCalling     native provider function-calling loop (modern default)
//   ReAct           text "Thought/Action/Observation" parsing (fallback for
//                   models without native tool use)
//   PlanAndExecute  plan upfront, then execute steps (Phase 4)
enum class Strategy { ToolCalling, ReAct, PlanAndExecute };

}  // namespace corvus
