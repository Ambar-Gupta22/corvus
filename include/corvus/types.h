#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace corvus {

// Shared value types used across the tool, memory, and client interfaces.
// Kept in one dependency-light header so tool.h / memory.h / llm_client.h
// never need to include each other.

// A tool invocation the model asked for.
struct ToolCall {
    std::string id;         // provider-assigned id (echoed back with the result)
    std::string name;       // tool name
    std::string arguments;  // JSON object string
};

// A single conversation turn kept in agent memory. Carries enough to
// reconstruct the exact provider wire format: an assistant turn keeps the
// tool calls it requested; a tool turn keeps the id it answers.
struct Message {
    std::string role;                 // "system" | "user" | "assistant" | "tool"
    std::string content;              // text payload
    std::string name;                 // role=="tool": which tool produced this
    std::string toolCallId;           // role=="tool": pairs result with request
    std::vector<ToolCall> toolCalls;  // role=="assistant": calls it requested
};

// CancelToken — cooperative cancellation. Copyable; all copies share one
// flag, so the caller can cancel a run from another thread.
class CancelToken {
public:
    void cancel() { flag_->store(true); }
    bool cancelled() const { return flag_->load(); }

private:
    std::shared_ptr<std::atomic<bool>> flag_ = std::make_shared<std::atomic<bool>>(false);
};

// ToolContext — per-call execution context handed to every tool. Tools that
// can block (network, subprocess, disk) must check `cancel` and `deadline`
// cooperatively; C++ cannot force-stop a thread, so ignoring them means a
// timeout can only abandon the call, not reclaim it.
struct ToolContext {
    CancelToken cancel;                                  // observe cooperatively
    std::chrono::steady_clock::time_point deadline{};    // zero = no deadline

    bool expired() const {
        return deadline != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() >= deadline;
    }
};

// ToolResult — what a tool execution produced. The status enum is what the
// agent loop branches on (retry vs give up); the model only ever sees the
// rendered observation text.
struct ToolResult {
    enum class Status { Ok, RetryableError, FatalError, Timeout, Cancelled };

    Status status = Status::Ok;
    std::string content;  // observation text, or the error explanation

    static ToolResult ok(std::string s) { return {Status::Ok, std::move(s)}; }
    static ToolResult retryable(std::string why) { return {Status::RetryableError, std::move(why)}; }
    static ToolResult fatal(std::string why) { return {Status::FatalError, std::move(why)}; }
};

}  // namespace corvus
