#include "corvus/agent.h"

namespace corvus {

Agent::Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
             int maxIterations)
    : llm_(std::move(llm)),
      registry_(std::move(registry)),
      memory_(std::move(memory)),
      strategy_(strategy),
      maxIterations_(maxIterations) {}

RunResult Agent::run(const std::string& task, const AgentCallbacks& callbacks) {
    CancelToken none;
    return runImpl(task, callbacks, none);
}

std::future<RunResult> Agent::runAsync(const std::string& task, CancelToken token,
                                       AgentCallbacks callbacks) {
    // NOTE: `this` is captured — the Agent must outlive the returned future.
    return std::async(std::launch::async, [this, task, token, callbacks]() {
        return runImpl(task, callbacks, token);
    });
}

RunResult Agent::runImpl(const std::string& task, const AgentCallbacks& callbacks,
                         const CancelToken& token) {
    RunResult result;

    // Snapshot the tool specs once — they don't change mid-run.
    std::vector<ToolSpec> specs;
    for (const auto& tool : registry_->all()) {
        ToolSpec spec;
        spec.name = tool->name();
        spec.description = tool->description();
        spec.parametersJson = tool->inputSchema();
        specs.push_back(std::move(spec));
    }

    memory_->append(Message{"user", task, ""});

    // Strategy currently routes through the native tool-calling loop. ReAct /
    // PlanAndExecute specialize this in later phases.
    for (int i = 0; i < maxIterations_; ++i) {
        if (token.cancelled()) {
            result.output = "[cancelled]";
            result.iterations = i;
            result.completed = false;
            return result;
        }

        result.iterations = i + 1;
        if (callbacks.onStep) {
            callbacks.onStep(i + 1);
        }

        LLMResponse response = llm_->complete(memory_->context(), specs, callbacks.onToken);

        if (response.toolCalls.empty()) {
            memory_->append(Message{"assistant", response.text, ""});
            result.output = response.text;
            result.completed = true;
            return result;
        }

        if (!response.text.empty()) {
            memory_->append(Message{"assistant", response.text, ""});
        }

        for (const ToolCall& call : response.toolCalls) {
            if (callbacks.onToolCall) {
                callbacks.onToolCall(call);
            }

            std::string observation;
            ToolPtr tool = registry_->get(call.name);
            if (!tool) {
                observation = "ERROR: unknown tool '" + call.name + "'";
            } else {
                observation = tool->execute(call.arguments);  // never throws by contract
            }

            if (callbacks.onToolResult) {
                callbacks.onToolResult(call.name, observation);
            }

            Message toolMsg;
            toolMsg.role = "tool";
            toolMsg.content = observation;
            toolMsg.name = call.name;
            memory_->append(toolMsg);
        }
    }

    // Reached the iteration cap without a final answer (loop guard).
    result.completed = false;
    if (result.output.empty()) {
        result.output = "[stopped: reached maxIterations]";
    }
    return result;
}

}  // namespace corvus
