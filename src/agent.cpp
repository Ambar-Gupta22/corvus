#include "corvus/agent.h"

#include <cstddef>
#include <stdexcept>
#include <string>

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

std::string truncationMarker(std::size_t removed) {
    return "\n[...truncated " + std::to_string(removed) + " bytes...]\n";
}

bool isUtf8Continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Cuts an over-cap observation to its head and tail with a marker between,
// because errors and conclusions usually sit at the end of tool output. Cut
// points land on UTF-8 code-point boundaries so the result stays valid UTF-8.
// The result fits in `cap`, except when `cap` can't even hold the marker:
// then it is the marker alone. Returns true if it truncated.
bool capObservation(std::string& s, std::size_t cap) {
    if (s.size() <= cap) {
        return false;
    }
    // The real marker counts fewer removed bytes than s.size(), so its length
    // is at most this one's: budgeting with it keeps the result within cap.
    const std::size_t markerMax = truncationMarker(s.size()).size();
    if (cap <= markerMax) {
        s = truncationMarker(s.size());
        return true;
    }
    const std::size_t budget = cap - markerMax;
    std::size_t headEnd = budget - budget / 2;
    std::size_t tailStart = s.size() - budget / 2;
    while (headEnd > 0 && isUtf8Continuation(s[headEnd])) {
        --headEnd;
    }
    while (tailStart < s.size() && isUtf8Continuation(s[tailStart])) {
        ++tailStart;
    }
    s = s.substr(0, headEnd) + truncationMarker(tailStart - headEnd) + s.substr(tailStart);
    return true;
}

// Runs the gate, failing closed: a throwing gate denies the call, and its
// exception text never reaches the model.
ToolDecision decide(const ToolGate& gate, const ToolCall& call, const CancelToken& token) {
    if (!gate) {
        return ToolDecision::allow();
    }
    try {
        return gate(call, token);
    } catch (...) {
        return ToolDecision::deny("tool gate failed");
    }
}

AgentLimits limitsWithIterations(int maxIterations) {
    AgentLimits limits;
    limits.maxIterations = maxIterations;
    return limits;
}

}  // namespace

Agent::Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
             int maxIterations)
    : Agent(std::move(llm), std::move(registry), std::move(memory), strategy,
            limitsWithIterations(maxIterations)) {}

Agent::Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
             AgentLimits limits, ToolGate gate)
    : state_(std::make_shared<State>()) {
    state_->llm = std::move(llm);
    state_->registry = std::move(registry);
    state_->memory = std::move(memory);
    state_->strategy = strategy;
    state_->limits = limits;
    state_->gate = std::move(gate);
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
    const AgentLimits& limits = state->limits;
    int callsThisRun = 0;  // processed calls, counted against maxToolCallsPerRun

    for (int i = 0; i < limits.maxIterations; ++i) {
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

        // Every requested call gets exactly one paired result, even when it is
        // skipped or denied: providers reject an unanswered tool call. Check
        // order: per-turn cap -> per-run cap -> lookup -> gate -> execute.
        for (std::size_t k = 0; k < response.toolCalls.size(); ++k) {
            const ToolCall& call = response.toolCalls[k];
            if (callbacks.onToolCall) {
                callbacks.onToolCall(call);
            }

            ToolResult toolResult;
            if (k >= static_cast<std::size_t>(limits.maxToolCallsPerTurn)) {
                toolResult = ToolResult::fatal("skipped: per-turn tool call limit (" +
                                               std::to_string(limits.maxToolCallsPerTurn) +
                                               ") reached");
                ++result.toolCallsSkipped;
            } else if (callsThisRun >= limits.maxToolCallsPerRun) {
                toolResult = ToolResult::fatal("skipped: per-run tool call limit (" +
                                               std::to_string(limits.maxToolCallsPerRun) +
                                               ") reached");
                ++result.toolCallsSkipped;
            } else {
                ++callsThisRun;
                ToolPtr tool = state->registry->get(call.name);
                if (!tool) {
                    toolResult = ToolResult::fatal("unknown tool '" + call.name + "'");
                } else {
                    const ToolDecision decision = decide(state->gate, call, token);
                    if (!decision.allowed) {
                        toolResult = ToolResult::fatal(
                            decision.reason.empty() ? "denied" : "denied: " + decision.reason);
                        ++result.toolCallsDenied;
                    } else {
                        ToolContext ctx;
                        ctx.cancel = token;
                        // ctx.deadline stays unset for now; the per-tool
                        // timeout watchdog stamps it in Phase 1.
                        toolResult = tool->execute(call.arguments, ctx);  // never throws
                        ++result.toolCallsExecuted;
                    }
                }
            }

            std::string observation = renderObservation(toolResult);
            if (capObservation(observation, limits.maxObservationBytes)) {
                ++result.observationsTruncated;
            }
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
