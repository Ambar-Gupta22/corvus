#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "corvus/types.h"

namespace corvus {

// Tool — the single extension contract. Built-in tools, user-defined C++
// tools, and MCP-adapted tools all implement this and live side by side in
// one ToolRegistry, treated identically by the agent.
//
// Hard rules:
//  - execute() must NEVER throw. On failure return ToolResult::retryable()
//    or ToolResult::fatal() — the loop branches on the status.
//  - Tools that can block (network, subprocess, disk) must honor
//    ctx.cancel / ctx.deadline cooperatively. C++ cannot force-stop a
//    thread; a tool that ignores its context can only be abandoned on
//    timeout, never reclaimed.
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

    // Execute with args as a JSON object string. NEVER throws.
    virtual ToolResult execute(const std::string& args, const ToolContext& ctx) = 0;
};

using ToolPtr = std::shared_ptr<Tool>;

// FunctionTool — low-boilerplate adapter so a simple tool needs no subclass.
// Two authoring forms (see makeTool below):
//  - simple:  std::string(const std::string& args) — return value becomes an
//    Ok result; a thrown exception becomes a fatal error.
//  - full:    ToolResult(const std::string& args, const ToolContext& ctx) —
//    for tools that report retryable errors or honor cancel/deadline.
class FunctionTool : public Tool {
public:
    using SimpleFn = std::function<std::string(const std::string& args)>;
    using Fn = std::function<ToolResult(const std::string& args, const ToolContext& ctx)>;

    FunctionTool(std::string name, std::string description, std::string schema, Fn fn)
        : name_(std::move(name)),
          description_(std::move(description)),
          schema_(std::move(schema)),
          fn_(std::move(fn)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return description_; }
    std::string inputSchema() const override { return schema_; }

    ToolResult execute(const std::string& args, const ToolContext& ctx) override {
        // Enforce the never-throw contract on behalf of the lambda author.
        try {
            return fn_(args, ctx);
        } catch (const std::exception& e) {
            return ToolResult::fatal(e.what());
        } catch (...) {
            return ToolResult::fatal("unknown exception in tool");
        }
    }

private:
    std::string name_;
    std::string description_;
    std::string schema_;
    Fn fn_;
};

// makeTool — author a tool in one expression, no class required.

// Full form: the body receives the execution context and returns a typed
// result, so it can report retryable errors and honor cancel/deadline.
inline ToolPtr makeTool(std::string name, std::string description, std::string schema,
                        FunctionTool::Fn fn) {
    return std::make_shared<FunctionTool>(std::move(name), std::move(description),
                                          std::move(schema), std::move(fn));
}

// Simple form: a plain string-in/string-out body for the common case. The
// return value becomes an Ok result; a thrown exception becomes fatal.
inline ToolPtr makeTool(std::string name, std::string description, std::string schema,
                        FunctionTool::SimpleFn fn) {
    FunctionTool::Fn wrapped = [fn = std::move(fn)](const std::string& args,
                                                    const ToolContext& /*ctx*/) -> ToolResult {
        return ToolResult::ok(fn(args));
    };
    return std::make_shared<FunctionTool>(std::move(name), std::move(description),
                                          std::move(schema), std::move(wrapped));
}

}  // namespace corvus
