// Placeholder backend factories. The real Anthropic / OpenAI / Ollama clients
// (HTTP + native tool calling + streaming) land in Phase 1. Until then these
// throw a clear message so the library links and MockLLM-based tests run.
#include <stdexcept>

#include "corvus/llm_client.h"

namespace corvus {

LLMClientPtr anthropic(const std::string& /*model*/, const std::string& /*key*/) {
    throw std::runtime_error("corvus: anthropic() backend lands in Phase 1 — use MockLLM for now");
}

LLMClientPtr openai(const std::string& /*model*/, const std::string& /*key*/) {
    throw std::runtime_error("corvus: openai() backend lands in Phase 1 — use MockLLM for now");
}

LLMClientPtr ollama(const std::string& /*model*/, const std::string& /*host*/) {
    throw std::runtime_error("corvus: ollama() backend lands in Phase 1 — use MockLLM for now");
}

}  // namespace corvus
