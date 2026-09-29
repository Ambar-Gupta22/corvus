#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "corvus/types.h"

namespace corvus {

// Memory — the agent's running context. Implementations decide how much to
// keep and how to window/summarize once the token budget is exceeded.
// Bounding (lastN / maxTokens / summaries) plugs in behind this same seam as
// opt-in policies — see docs/specs/2026-07-04-memory-design.md.
class Memory {
public:
    virtual ~Memory() = default;
    // Record one turn at the end of the history.
    virtual void append(const Message& message) = 0;
    // The messages to send the model on the next turn, oldest first (a copy).
    virtual std::vector<Message> context() const = 0;
    // Forget everything.
    virtual void clear() = 0;
};

using MemoryPtr = std::shared_ptr<Memory>;

// InMemoryMemory — keeps the full, unbounded history in RAM. The default.
// Thread-safe (one internal mutex). SqliteMemory (persistent) arrives in Phase 1.
class InMemoryMemory : public Memory {
public:
    void append(const Message& message) override;
    std::vector<Message> context() const override;
    void clear() override;

private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
};

// Factory for a fresh, empty InMemoryMemory.
MemoryPtr inMemory();

}  // namespace corvus
