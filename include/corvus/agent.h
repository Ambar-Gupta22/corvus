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

namespace corvus {

// CancelToken — cooperative cancellation for runAsync(). Copyable; all copies
// share one flag, so the caller can cancel a run from another thread.
class CancelToken {
public:
    void cancel() { flag_->store(true); }
    bool cancelled() const { return flag_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_ = std::make_shared<std::atomic<bool>>(false);
};

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
    std::string output;       // final answer (or last text on early stop)
    int iterations = 0;       // loop iterations consumed
    bool completed = false;   // false if it hit maxIterations or was cancelled
};

// Agent — owns one reasoning loop over a model + tools + memory. Construct via
// AgentBuilder.
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
    RunResult runImpl(const std::string& task, const AgentCallbacks& callbacks,
                      const CancelToken& token);

    LLMClientPtr llm_;
    ToolRegistryPtr registry_;
    MemoryPtr memory_;
    Strategy strategy_;
    int maxIterations_;
};

}  // namespace corvus
