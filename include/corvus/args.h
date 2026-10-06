#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace corvus {

// Args — read-only view over a tool's arguments, parsed from the JSON object
// text the model sent. Lets tool authors read typed values without bringing
// their own JSON library (the parser lives in src/, never in this header).
//
//   std::string err;
//   auto args = corvus::Args::parse(text, &err);
//   if (!args) return ToolResult::retryable("invalid arguments: " + err);
//   auto city = args->getString("city");
//
// Getters are strict: a value of the wrong JSON type yields nullopt (no
// string->number coercion). A key holding JSON null counts as absent.
class Args {
public:
    // Parse a JSON object. Empty text is treated as {} (a no-arg tool call).
    // Returns nullopt for invalid JSON or a non-object top level, writing a
    // short model-readable reason to *error when non-null.
    static std::optional<Args> parse(const std::string& text, std::string* error = nullptr);

    Args(Args&&) noexcept;
    Args& operator=(Args&&) noexcept;
    Args(const Args&) = delete;
    Args& operator=(const Args&) = delete;
    ~Args();

    // True when `key` is present and not null.
    bool has(const std::string& key) const;

    std::optional<std::string> getString(const std::string& key) const;

    // Any JSON number (integer or float).
    std::optional<double> getNumber(const std::string& key) const;

    // A JSON integer in int64 range, or a float that is exactly integral
    // (some providers serialise every number as a float, e.g. 3.0).
    std::optional<std::int64_t> getInteger(const std::string& key) const;

    std::optional<bool> getBool(const std::string& key) const;

private:
    struct Impl;
    explicit Args(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace corvus
