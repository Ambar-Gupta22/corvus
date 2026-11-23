#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "corvus/llm_client.h"
#include "corvus/memory.h"
#include "corvus/strategy.h"
#include "corvus/tool_registry.h"
#include "corvus/types.h"

namespace corvus {

// Observability + streaming hooks. All optional. onStep doubles as the
// lightweight tracing seam (one call per reason->act->observe iteration).
struct AgentCallbacks {
    TokenCallback onToken;  // streamed text
    // Every call the model requested, before any limit, lookup or gate check.
    std::function<void(const ToolCall& call)> onToolCall;
    // Every requested call's final observation, exactly as the model will see
    // it — including skipped, denied and truncated ones.
    std::function<void(const std::string& tool, const std::string& result)> onToolResult;
    std::function<void(int iteration)> onStep;  // per loop iteration
};

struct RunResult {
    std::string output;       // final answer, or the last assistant text on early stop
    int iterations = 0;       // loop iterations consumed
    bool completed = false;   // false if it hit maxIterations or was cancelled
    int toolCallsExecuted = 0;      // calls that reached Tool::execute
    int toolCallsDenied = 0;        // calls the tool gate refused (or that made it throw)
    int toolCallsSkipped = 0;       // calls over the per-turn or per-run limit
    int observationsTruncated = 0;  // results cut to maxObservationBytes
};

// ToolDecision — a tool gate's verdict on one call.
struct ToolDecision {
    bool allowed = true;
    // On deny, shown to the model verbatim as "ERROR: denied: <reason>" —
    // never put secrets or internal detail here.
    std::string reason;

    static ToolDecision allow() { return {}; }
    static ToolDecision deny(std::string why = "") { return {false, std::move(why)}; }
};

// Tool gate — developer code that sees each tool call before it runs and may
// deny it (geofence, confirmation dialog, human approval). Optional: with no
// gate every call is allowed. The model can never see or change it. Runs on
// the run's thread, so a gate shared between agents must be thread-safe.
// If it throws, the call is denied (fail closed).
using SimpleToolGate = std::function<ToolDecision(const ToolCall& call)>;
// Cancel-aware form, for gates that block (e.g. waiting on a human): watch
// `cancel`, or a cancelled run waits on the gate forever.
using ToolGate = std::function<ToolDecision(const ToolCall& call, const CancelToken& cancel)>;

// Hard bounds on one run, whatever the model sends back. All on by default;
// each must be >= 1.
struct AgentLimits {
    int maxIterations = 10;                        // model turns
    int maxToolCallsPerTurn = 16;                  // calls processed per model response
    int maxToolCallsPerRun = 64;                   // calls processed per run
    std::size_t maxObservationBytes = 64 * 1024;   // per tool result, head + tail kept
};

// Agent — owns one reasoning loop over a model + tools + memory. Construct via
// AgentBuilder.
//
// Semantics:
//  - An Agent is a cheap HANDLE: copying it copies a handle to the same agent
//    and the same conversation, not an independent agent.
//  - One run at a time. Starting a run while another is in flight throws
//    std::logic_error (surfaced at future.get() for runAsync). For concurrent
//    tasks, build a second Agent.
//  - Lifetime-safe async: the future returned by runAsync() keeps the agent's
//    state alive on its own, so the Agent object may be moved or destroyed
//    while the run is in flight.
class Agent {
public:
    // Prefer AgentBuilder, which validates and fills defaults; these
    // constructors trust their arguments (all non-null, every limit >= 1).
    // The first uses the default AgentLimits apart from maxIterations.
    Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
          int maxIterations);
    Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
          AgentLimits limits, ToolGate gate = nullptr);

    // Blocking convenience. Streams/traces through `callbacks` if provided;
    // callbacks run on the calling thread. Throws std::logic_error if a run is
    // already in flight; exceptions from the LLMClient propagate.
    RunResult run(const std::string& task, const AgentCallbacks& callbacks = {});

    // Non-blocking: returns immediately with a future. Pass a CancelToken to
    // stop it cooperatively (e.g. from a ROS2 spin loop or game thread).
    // Callbacks run on the worker thread, so they must be thread-safe.
    // Cancellation is checked at the start of each loop iteration.
    std::future<RunResult> runAsync(const std::string& task, CancelToken token = {},
                                    AgentCallbacks callbacks = {});

private:
    // Everything a run needs, owned by shared_ptr so an in-flight run keeps
    // it alive independently of the Agent object.
    struct State {
        LLMClientPtr llm;
        ToolRegistryPtr registry;
        MemoryPtr memory;
        Strategy strategy;
        AgentLimits limits;
        ToolGate gate;  // empty = allow every call
        std::atomic<bool> running{false};
    };

    static RunResult runImpl(const std::shared_ptr<State>& state, const std::string& task,
                             const AgentCallbacks& callbacks, const CancelToken& token);

    std::shared_ptr<State> state_;
};

}  // namespace corvus
