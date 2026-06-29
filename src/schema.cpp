#include "corvus/schema.h"

namespace corvus {

namespace {

// Minimal JSON string escaping — enough for tool names/descriptions.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += c;
        }
    }
    return out;
}

}  // namespace

std::string Schema::json() const {
    // Produces a JSON Schema "object": { type, properties{...}, required[...] }
    std::string out = "{\"type\":\"object\",\"properties\":{";

    bool first = true;
    for (const auto& f : fields_) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "\"" + escape(f.name) + "\":{\"type\":\"" + f.type + "\",\"description\":\"" +
               escape(f.description) + "\"}";
    }
    out += "}";

    // required array
    std::string required;
    bool firstReq = true;
    for (const auto& f : fields_) {
        if (!f.required) {
            continue;
        }
        if (!firstReq) {
            required += ",";
        }
        firstReq = false;
        required += "\"" + escape(f.name) + "\"";
    }
    out += ",\"required\":[" + required + "]}";

    return out;
}

}  // namespace corvus
