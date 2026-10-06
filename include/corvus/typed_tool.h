#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "corvus/args.h"
#include "corvus/schema.h"
#include "corvus/tool.h"
#include "corvus/types.h"

namespace corvus {

// Whether the model must supply a field. Named so call sites read clearly
// instead of carrying a bare bool.
enum class Requiredness { Required, Optional };
inline constexpr Requiredness Required = Requiredness::Required;
inline constexpr Requiredness Optional = Requiredness::Optional;

namespace detail {

template <class M>
struct OptionalTraits {
    static constexpr bool isOptional = false;
    using Value = M;
};
template <class U>
struct OptionalTraits<std::optional<U>> {
    static constexpr bool isOptional = true;
    using Value = U;
};

// Character types are integral but never meant as JSON integers.
template <class V>
inline constexpr bool kIsCharType = std::is_same_v<V, char> || std::is_same_v<V, wchar_t> ||
                                    std::is_same_v<V, char16_t> || std::is_same_v<V, char32_t>;

template <class V>
inline constexpr bool kIsIntegerField =
    std::is_integral_v<V> && !std::is_same_v<V, bool> && !kIsCharType<V>;

template <class V>
inline constexpr bool kIsSupportedField = std::is_same_v<V, std::string> ||
                                          std::is_same_v<V, bool> || kIsIntegerField<V> ||
                                          std::is_floating_point_v<V>;

// Read args[key] (known present) into `out`. On a type mismatch, writes a
// model-readable reason to `err` and returns false.
template <class V>
bool readField(const Args& args, const std::string& key, V& out, std::string& err) {
    if constexpr (std::is_same_v<V, std::string>) {
        auto v = args.getString(key);
        if (!v) {
            err = "field '" + key + "' must be a string";
            return false;
        }
        out = std::move(*v);
    } else if constexpr (std::is_same_v<V, bool>) {
        auto v = args.getBool(key);
        if (!v) {
            err = "field '" + key + "' must be a boolean";
            return false;
        }
        out = *v;
    } else if constexpr (kIsIntegerField<V>) {
        auto v = args.getInteger(key);
        bool inRange = false;
        if (v) {
            if constexpr (std::is_unsigned_v<V>) {
                inRange = *v >= 0 && static_cast<std::uint64_t>(*v) <=
                                         static_cast<std::uint64_t>(std::numeric_limits<V>::max());
            } else {
                inRange = *v >= static_cast<std::int64_t>(std::numeric_limits<V>::min()) &&
                          *v <= static_cast<std::int64_t>(std::numeric_limits<V>::max());
            }
        }
        if (!inRange) {
            err = "field '" + key + "' must be an integer in [" +
                  std::to_string(std::numeric_limits<V>::min()) + ", " +
                  std::to_string(std::numeric_limits<V>::max()) + "]";
            return false;
        }
        out = static_cast<V>(*v);
    } else {
        auto v = args.getNumber(key);
        if (!v || *v > std::numeric_limits<V>::max() || *v < std::numeric_limits<V>::lowest()) {
            err = "field '" + key + "' must be a number";
            return false;
        }
        out = static_cast<V>(*v);
    }
    return true;
}

}  // namespace detail

// TypedToolBuilder — author a tool whose args arrive as a C++ struct. Each
// field() call records one member: its JSON Schema entry (type deduced from
// the member type) and how to read it. Schema and parsing come from that one
// list, so they cannot drift apart.
//
//   struct MoveArgs { double x = 0; double y = 0; std::string frame = "map"; };
//
//   auto move = corvus::typedTool<MoveArgs>("move_to", "Move the robot to (x, y)")
//       .field("x", &MoveArgs::x, "target x in metres")
//       .field("y", &MoveArgs::y, "target y in metres")
//       .field("frame", &MoveArgs::frame, "TF frame", corvus::Optional)
//       .run([&](const MoveArgs& a) { return robot.moveTo(a.x, a.y, a.frame); });
//
// Supported member types: std::string, bool, integers (range-checked),
// floating point, and std::optional of those (always optional; absent ->
// nullopt). An absent optional field keeps the member's default initializer.
// Bad args from the model become a RetryableError naming the field, so the
// model can correct itself. Unknown keys are ignored.
template <class T>
class TypedToolBuilder {
    static_assert(std::is_default_constructible_v<T>,
                  "typedTool<T>: T must be default-constructible");

public:
    TypedToolBuilder(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description)) {}

    // Register a member. Throws std::invalid_argument on an empty or
    // duplicate key (fail fast at construction, not at call time).
    template <class M>
    TypedToolBuilder& field(std::string key, M T::*member, const std::string& description,
                            Requiredness requiredness = Requiredness::Required) {
        using Traits = detail::OptionalTraits<M>;
        using V = typename Traits::Value;
        static_assert(detail::kIsSupportedField<V>,
                      "typedTool field: member type must be std::string, bool, an integer, a "
                      "floating-point type, or std::optional of one of those");

        if (key.empty()) {
            throw std::invalid_argument("typedTool '" + name_ + "': field key must not be empty");
        }
        if (!keys_.insert(key).second) {
            throw std::invalid_argument("typedTool '" + name_ + "': duplicate field '" + key +
                                        "'");
        }

        const bool required = !Traits::isOptional && requiredness == Requiredness::Required;
        if constexpr (std::is_same_v<V, std::string>) {
            schema_.str(key, description, required);
        } else if constexpr (std::is_same_v<V, bool>) {
            schema_.boolean(key, description, required);
        } else if constexpr (detail::kIsIntegerField<V>) {
            schema_.integer(key, description, required);
        } else {
            schema_.num(key, description, required);
        }

        binders_.push_back([key = std::move(key), member, required](
                               const Args& args, T& out, std::string& err) -> bool {
            if (!args.has(key)) {
                if (required) {
                    err = "missing required field '" + key + "'";
                    return false;
                }
                return true;  // keep the default
            }
            if constexpr (Traits::isOptional) {
                V value{};
                if (!detail::readField(args, key, value, err)) {
                    return false;
                }
                out.*member = std::move(value);
                return true;
            } else {
                return detail::readField(args, key, out.*member, err);
            }
        });
        return *this;
    }

    // The JSON Schema the model will see (same text the built tool reports).
    std::string inputSchema() const { return schema_.json(); }

    // Finish the tool. `fn` is either
    //   std::string(const T&)                        -> Ok result, or
    //   ToolResult(const T&, const ToolContext&)     -> returned as-is
    // (the second form can report retryable errors and honor cancel/deadline).
    template <class Fn>
    ToolPtr run(Fn fn) const {
        static_assert(kIsFullForm<Fn> || kIsSimpleForm<Fn>,
                      "typedTool run(): fn must be std::string(const T&) or "
                      "ToolResult(const T&, const ToolContext&)");

        FunctionTool::Fn body = [binders = binders_, fn = std::move(fn)](
                                    const std::string& text,
                                    const ToolContext& ctx) mutable -> ToolResult {
            std::string err;
            auto args = Args::parse(text, &err);
            if (!args) {
                return ToolResult::retryable("invalid arguments: " + err);
            }
            T value{};
            for (const auto& bind : binders) {
                if (!bind(*args, value, err)) {
                    return ToolResult::retryable("invalid arguments: " + err);
                }
            }
            if constexpr (kIsFullForm<Fn>) {
                return fn(value, ctx);
            } else {
                (void)ctx;
                return ToolResult::ok(fn(value));
            }
        };
        return makeTool(name_, description_, schema_.json(), std::move(body));
    }

private:
    template <class Fn>
    static constexpr bool kIsFullForm =
        std::is_invocable_r_v<ToolResult, Fn&, const T&, const ToolContext&>;
    template <class Fn>
    static constexpr bool kIsSimpleForm = std::is_invocable_r_v<std::string, Fn&, const T&>;

    using Binder = std::function<bool(const Args&, T&, std::string&)>;

    std::string name_;
    std::string description_;
    Schema schema_;
    std::set<std::string> keys_;
    std::vector<Binder> binders_;
};

// Entry point: corvus::typedTool<MyArgs>("name", "description").field(...).run(...)
template <class T>
TypedToolBuilder<T> typedTool(std::string name, std::string description) {
    return TypedToolBuilder<T>(std::move(name), std::move(description));
}

}  // namespace corvus
