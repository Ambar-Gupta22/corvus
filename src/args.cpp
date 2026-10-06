#include "corvus/args.h"

#include <cmath>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

namespace corvus {

struct Args::Impl {
    nlohmann::json obj;  // always an object
};

Args::Args(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Args::Args(Args&&) noexcept = default;
Args& Args::operator=(Args&&) noexcept = default;
Args::~Args() = default;

std::optional<Args> Args::parse(const std::string& text, std::string* error) {
    auto fail = [error](std::string why) -> std::optional<Args> {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return std::nullopt;
    };

    auto impl = std::make_unique<Impl>();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        impl->obj = nlohmann::json::object();
        return Args(std::move(impl));
    }

    impl->obj = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (impl->obj.is_discarded()) {
        return fail("arguments are not valid JSON");
    }
    if (!impl->obj.is_object()) {
        return fail("arguments must be a JSON object");
    }
    return Args(std::move(impl));
}

namespace {

// The value at `key`, or nullptr when absent or null.
const nlohmann::json* lookup(const nlohmann::json& obj, const std::string& key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return nullptr;
    }
    return &*it;
}

}  // namespace

bool Args::has(const std::string& key) const { return lookup(impl_->obj, key) != nullptr; }

std::optional<std::string> Args::getString(const std::string& key) const {
    const auto* v = lookup(impl_->obj, key);
    if (v == nullptr || !v->is_string()) {
        return std::nullopt;
    }
    return v->get<std::string>();
}

std::optional<double> Args::getNumber(const std::string& key) const {
    const auto* v = lookup(impl_->obj, key);
    if (v == nullptr || !v->is_number()) {
        return std::nullopt;
    }
    return v->get<double>();
}

std::optional<std::int64_t> Args::getInteger(const std::string& key) const {
    const auto* v = lookup(impl_->obj, key);
    if (v == nullptr) {
        return std::nullopt;
    }
    if (v->is_number_unsigned()) {
        auto u = v->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(u);
    }
    if (v->is_number_integer()) {
        return v->get<std::int64_t>();
    }
    if (v->is_number_float()) {
        double d = v->get<double>();
        // [-2^63, 2^63): both bounds are exact in double.
        if (!std::isfinite(d) || d != std::trunc(d) || d < -9223372036854775808.0 ||
            d >= 9223372036854775808.0) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(d);
    }
    return std::nullopt;
}

std::optional<bool> Args::getBool(const std::string& key) const {
    const auto* v = lookup(impl_->obj, key);
    if (v == nullptr || !v->is_boolean()) {
        return std::nullopt;
    }
    return v->get<bool>();
}

}  // namespace corvus
