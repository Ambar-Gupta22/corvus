#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "corvus/memory.h"

namespace corvus {

// A tool invocation the model asked for.
struct ToolCall {
    std::string id;         // provider-assigned id (echoed back with the result)
    std::string name;       // tool name
    std::string arguments;  // JSON object string
};

// One model turn. If toolCalls is empty, `text` is the final answer.
struct LLMResponse {
    std::string text;
    std::vector<ToolCall> toolCalls;
};

// Tool description handed to the model so it knows what it may call.
struct ToolSpec {
    std::string name;
    std::string description;
    std::string parametersJson;  // JSON Schema string
};

// Streaming callback: invoked per token/chunk as the model emits text.
using TokenCallback = std::function<void(const std::string& chunk)>;

// LLMClient — the backend abstraction. Anthropic/OpenAI/Ollama/llama.cpp
// implement this. Phase 1 wires the real HTTP/native backends; Phase 0 ships
// MockLLM for deterministic, offline, key-free tests.
class LLMClient {
public:
    virtual ~LLMClient() = default;
    virtual std::string name() const = 0;

    // One completion turn. `tools` may be empty. `onToken`, if set, receives
    // streamed text chunks. Implementations must support native tool calling.
    virtual LLMResponse complete(const std::vector<Message>& messages,
                                 const std::vector<ToolSpec>& tools,
                                 const TokenCallback& onToken = nullptr) = 0;
};

using LLMClientPtr = std::shared_ptr<LLMClient>;

// Backend factories. Implemented in Phase 1 — declared now so the public API
// is stable from the start.
LLMClientPtr anthropic(const std::string& model, const std::string& key = "");
LLMClientPtr openai(const std::string& model, const std::string& key = "");
LLMClientPtr ollama(const std::string& model, const std::string& host = "http://localhost:11434");

}  // namespace corvus
