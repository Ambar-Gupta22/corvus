#include "corvus/agent.h"

#include <stdexcept>

namespace corvus {

namespace {

// RAII guard: claims the agent's single-run slot, releases it on every exit
// path (normal return, cancellation, exception).
class RunningGuard {
public:
    explicit RunningGuard(std::atomic<bool>& flag) : flag_(flag) {
        if (flag_.exchange(true)) {
            throw std::logic_error(
                "corvus: this Agent is already running; agents are single-run — "
                "build a second Agent for concurrent tasks");
        }
    }
    ~RunningGuard() { flag_.store(false); }

    RunningGuard(const RunningGuard&) = delete;
    RunningGuard& operator=(const RunningGuard&) = delete;

private:
    std::atomic<bool>& flag_;
};

// Render a tool's typed result into the observation text the model sees.
// The status enum is loop-facing; "ERROR: ..." stays the model-facing shape.
std::string renderObservation(const ToolResult& result) {
    switch (result.status) {
        case ToolResult::Status::Ok:
            return result.content;
        case ToolResult::Status::Timeout:
            return "ERROR: tool timed out" + (result.content.empty() ? "" : ": " + result.content);
        case ToolResult::Status::Cancelled:
            return "ERROR: tool cancelled" + (result.content.empty() ? "" : ": " + result.content);
        case ToolResult::Status::RetryableError:
        case ToolResult::Status::FatalError:
        default:
            return "ERROR: " + result.content;
    }
}

}  // namespace

Agent::Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
             int maxIterations)
    : state_(std::make_shared<State>()) {
    state_->llm = std::move(llm);
    state_->registry = std::move(registry);
    state_->memory = std::move(memory);
    state_->strategy = strategy;
    state_->maxIterations = maxIterations;
}

RunResult Agent::run(const std::string& task, const AgentCallbacks& callbacks) {
    CancelToken none;
    return runImpl(state_, task, callbacks, none);
}

std::future<RunResult> Agent::runAsync(const std::string& task, CancelToken token,
                                       AgentCallbacks callbacks) {
    // The lambda captures the shared state by value, so the run owns what it
    // needs: the Agent object may be moved or destroyed while this is in
    // flight. A concurrent-run logic_error surfaces at future.get().
    auto state = state_;
    return std::async(std::launch::async, [state, task, token, callbacks]() {
        return runImpl(state, task, callbacks, token);
    });
}

RunResult Agent::runImpl(const std::shared_ptr<State>& state, const std::string& task,
                         const AgentCallbacks& callbacks, const CancelToken& token) {
    RunningGuard guard(state->running);

    RunResult result;

    // Snapshot the tool specs once — they don't change mid-run.
    std::vector<ToolSpec> specs;
    for (const auto& tool : state->registry->all()) {
        ToolSpec spec;
        spec.name = tool->name();
        spec.description = tool->description();
        spec.parametersJson = tool->inputSchema();
        specs.push_back(std::move(spec));
    }

    Message userMsg;
    userMsg.role = "user";
    userMsg.content = task;
    state->memory->append(userMsg);

    // Strategy currently routes through the native tool-calling loop. ReAct /
    // PlanAndExecute specialize this in later phases (builder rejects them
    // until then).
    for (int i = 0; i < state->maxIterations; ++i) {
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

        LLMResponse response = state->llm->complete(state->memory->context(), specs,
                                                    callbacks.onToken);

        if (response.toolCalls.empty()) {
            Message finalMsg;
            finalMsg.role = "assistant";
            finalMsg.content = response.text;
            state->memory->append(finalMsg);
            result.output = response.text;
            result.completed = true;
            return result;
        }

        // Always record the assistant turn WITH the tool calls it requested
        // (even if its text is empty) — providers require this turn in the
        // transcript to pair each tool result by id.
        Message assistantMsg;
        assistantMsg.role = "assistant";
        assistantMsg.content = response.text;
        assistantMsg.toolCalls = response.toolCalls;
        state->memory->append(assistantMsg);
        if (!response.text.empty()) {
            result.output = response.text;  // best answer so far, if we stop early
        }

        for (const ToolCall& call : response.toolCalls) {
            if (callbacks.onToolCall) {
                callbacks.onToolCall(call);
            }

            ToolContext ctx;
            ctx.cancel = token;
            // ctx.deadline stays unset in Phase 0; the per-tool timeout
            // watchdog stamps it in Phase 1.

            ToolPtr tool = state->registry->get(call.name);
            ToolResult toolResult =
                tool ? tool->execute(call.arguments, ctx)  // never throws by contract
                     : ToolResult::fatal("unknown tool '" + call.name + "'");

            std::string observation = renderObservation(toolResult);
            if (callbacks.onToolResult) {
                callbacks.onToolResult(call.name, observation);
            }

            Message toolMsg;
            toolMsg.role = "tool";
            toolMsg.content = observation;
            toolMsg.name = call.name;
            toolMsg.toolCallId = call.id;  // pairs this result with the request
            state->memory->append(toolMsg);
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
