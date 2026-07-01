#pragma once

#include <functional>
#include <memory>
#include <string>

namespace corvus {

// Tool — the single extension contract. Built-in tools, user-defined C++
// tools, and MCP-adapted tools all implement this and live side by side in
// one ToolRegistry, treated identically by the agent.
//
// Hard rule: execute() must NEVER throw. On failure, return a string that
// begins with "ERROR: " — the agent reads it and decides whether to retry.
class Tool {
public:
    virtual ~Tool() = default;

    // Unique name the model uses to call this tool (e.g. "web_search").
    virtual std::string name() const = 0;

    // Human + model readable description. THE MODEL READS THIS to decide when
    // and how to call the tool. Quality here = the tool actually gets used.
    virtual std::string description() const = 0;

    // JSON Schema (as JSON text) for the args object. Default: no args.
    virtual std::string inputSchema() const { return "{}"; }

    // Execute with args as a JSON object string. Returns the observation text.
    // NEVER throws. On failure return "ERROR: <explanation>".
    virtual std::string execute(const std::string& args) = 0;
};

using ToolPtr = std::shared_ptr<Tool>;

// FunctionTool — low-boilerplate adapter so a simple tool needs no subclass.
// The body receives the raw JSON args string (parse it with the JSON lib of
// your choice) and returns the observation text.
class FunctionTool : public Tool {
public:
    using Fn = std::function<std::string(const std::string& args)>;

    FunctionTool(std::string name, std::string description, std::string schema, Fn fn)
        : name_(std::move(name)),
          description_(std::move(description)),
          schema_(std::move(schema)),
          fn_(std::move(fn)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    std::string inputSchema() const override { return schema_; }

    std::string execute(const std::string& args) override {
        // Enforce the never-throw contract on behalf of the lambda author.
        try {
            return fn_(args);
        } catch (const std::exception& e) {
            return std::string("ERROR: ") + e.what();
        } catch (...) {
            return "ERROR: unknown exception in tool";
        }
    }

private:
    std::string name_;
    std::string description_;
    std::string schema_;
    Fn fn_;
};

// makeTool — author a tool in one expression, no class required.
inline ToolPtr makeTool(std::string name, std::string description, std::string schema, FunctionTool::Fn fn) {
    return std::make_shared<FunctionTool>(std::move(name), std::move(description), std::move(schema), std::move(fn));
}

}  // namespace corvus
