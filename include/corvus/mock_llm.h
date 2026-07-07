#pragma once

#include <deque>
#include <string>
#include <vector>

#include "corvus/llm_client.h"

namespace corvus {

// MockLLM — deterministic backend for tests and examples. Queue the responses
// you want; complete() returns them in order. No network, no API key. This is
// the backbone of the offline test harness.
//
//   MockLLM llm;
//   llm.callTool("calc", R"({"expr":"2+2"})").reply("The answer is 4.");
class MockLLM : public LLMClient {
public:
    std::string name() const override { return "mock"; }

    // Queue a final text answer (no tool calls).
    MockLLM& reply(std::string text);

    // Queue a turn that asks for a single tool call.
    MockLLM& callTool(std::string toolName, std::string argumentsJson);

    // Queue a turn that returns text AND asks for a tool call (models often
    // narrate before acting) — lets tests exercise mid-run assistant text.
    MockLLM& replyAndCallTool(std::string text, std::string toolName, std::string argumentsJson);

    LLMResponse complete(const std::vector<Message>& messages,
                         const std::vector<ToolSpec>& tools,
                         const TokenCallback& onToken = nullptr) override;

private:
    std::deque<LLMResponse> queue_;
};

}  // namespace corvus
