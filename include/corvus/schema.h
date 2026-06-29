#pragma once

#include <string>
#include <vector>

namespace corvus {

// Schema — tiny fluent builder that produces a JSON Schema object string for a
// flat set of typed parameters, so tool authors never hand-write JSON.
//
//   corvus::schema().str("city", "city name").num("days", "forecast length", false)
//
// Implicitly converts to std::string, so it drops straight into makeTool(...).
class Schema {
public:
    Schema& str(const std::string& name, const std::string& description, bool required = true) {
        return add(name, "string", description, required);
    }
    Schema& num(const std::string& name, const std::string& description, bool required = true) {
        return add(name, "number", description, required);
    }
    Schema& integer(const std::string& name, const std::string& description, bool required = true) {
        return add(name, "integer", description, required);
    }
    Schema& boolean(const std::string& name, const std::string& description, bool required = true) {
        return add(name, "boolean", description, required);
    }

    // Build the JSON Schema object as text.
    std::string json() const;

    operator std::string() const { return json(); }

private:
    struct Field {
        std::string name;
        std::string type;
        std::string description;
        bool required;
    };

    Schema& add(const std::string& name, const std::string& type, const std::string& description,
                bool required) {
        fields_.push_back(Field{name, type, description, required});
        return *this;
    }

    std::vector<Field> fields_;
};

inline Schema schema() { return Schema(); }

}  // namespace corvus
