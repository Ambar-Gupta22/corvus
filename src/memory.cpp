#include "corvus/memory.h"

namespace corvus {

void InMemoryMemory::append(const Message& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(message);
}

std::vector<Message> InMemoryMemory::context() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_;
}

void InMemoryMemory::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.clear();
}

MemoryPtr inMemory() { return std::make_shared<InMemoryMemory>(); }

}  // namespace corvus
