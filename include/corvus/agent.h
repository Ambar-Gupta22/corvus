#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>

#include "corvus/llm_client.h"
#include "corvus/memory.h"
#include "corvus/strategy.h"
#include "corvus/tool_registry.h"
#include "corvus/types.h"

namespace corvus {

// Observability + streaming hooks. All optional. onStep doubles as the
// lightweight tracing seam (one call per reason->act->observe iteration).
struct AgentCallbacks {
    TokenCallback onToken;                                                    // streamed text
    std::function<void(const ToolCall& call)> onToolCall;                     // before a tool runs
    std::function<void(const std::string& tool, const std::string& result)>  // after a tool runs
        onToolResult;
    std::function<void(int iteration)> onStep;                               // per loop iteration
};

struct RunResult {
    std::string output;       // final answer, or the last assistant text on early stop
    int iterations = 0;       // loop iterations consumed
    bool completed = false;   // false if it hit maxIterations or was cancelled
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
    Agent(LLMClientPtr llm, ToolRegistryPtr registry, MemoryPtr memory, Strategy strategy,
          int maxIterations);

    // Blocking convenience. Streams/traces through `callbacks` if provided.
    RunResult run(const std::string& task, const AgentCallbacks& callbacks = {});

    // Non-blocking: returns immediately with a future. Pass a CancelToken to
    // stop it cooperatively (e.g. from a ROS2 spin loop or game thread).
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
        int maxIterations;
        std::atomic<bool> running{false};
    };

    static RunResult runImpl(const std::shared_ptr<State>& state, const std::string& task,
                             const AgentCallbacks& callbacks, const CancelToken& token);

    std::shared_ptr<State> state_;
};

}  // namespace corvus
