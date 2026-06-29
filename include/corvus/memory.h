#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corvus {

// A single conversation turn kept in agent memory.
struct Message {
    std::string role;     // "system" | "user" | "assistant" | "tool"
    std::string content;  // text payload
    std::string name;     // optional: tool name for role == "tool"
};

// Memory — the agent's running context. Implementations decide how much to
// keep and how to window/summarize once the token budget is exceeded.
class Memory {
public:
    virtual ~Memory() = default;
    virtual void append(const Message& message) = 0;
    virtual std::vector<Message> context() const = 0;
    virtual void clear() = 0;
};

using MemoryPtr = std::shared_ptr<Memory>;

// InMemoryMemory — keeps the full history in RAM. The default. SqliteMemory
// (persistent) arrives in Phase 1.
class InMemoryMemory : public Memory {
public:
    void append(const Message& message) override;
    std::vector<Message> context() const override;
    void clear() override;

private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
};

MemoryPtr inMemory();

}  // namespace corvus
