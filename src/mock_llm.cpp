#include "corvus/mock_llm.h"

namespace corvus {

MockLLM& MockLLM::reply(std::string text) {
    LLMResponse r;
    r.text = std::move(text);
    queue_.push_back(std::move(r));
    return *this;
}

MockLLM& MockLLM::callTool(std::string toolName, std::string argumentsJson) {
    return replyAndCallTool("", std::move(toolName), std::move(argumentsJson));
}

MockLLM& MockLLM::replyAndCallTool(std::string text, std::string toolName,
                                   std::string argumentsJson) {
    LLMResponse r;
    r.text = std::move(text);
    ToolCall call;
    call.id = "mock-call-" + std::to_string(queue_.size());
    call.name = std::move(toolName);
    call.arguments = std::move(argumentsJson);
    r.toolCalls.push_back(std::move(call));
    queue_.push_back(std::move(r));
    return *this;
}

LLMResponse MockLLM::complete(const std::vector<Message>& /*messages*/,
                              const std::vector<ToolSpec>& /*tools*/,
                              const TokenCallback& onToken) {
    if (queue_.empty()) {
        // Defensive default: a real backend always answers something.
        LLMResponse r;
        r.text = "[MockLLM] no queued response";
        return r;
    }
    LLMResponse r = queue_.front();
    queue_.pop_front();

    // Emulate streaming for the text path so onToken-based tests have signal.
    if (onToken && !r.text.empty()) {
        onToken(r.text);
    }
    return r;
}

}  // namespace corvus
