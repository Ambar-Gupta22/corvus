# Phase 0 Hardening — Design Spec

**Date:** 2026-07-06
**Status:** Awaiting approval
**Scope:** Fixes to Phase 0 public API and behavior, decided *before* first push so no adopter is ever broken by them. Comes from the full Phase 0 review (code + design doc + knowledge graph).

## Why this spec exists

Phase 0 works and its tests pass. But three of its public headers make promises that Phase 1 cannot keep (missing tool-call ids, a tool contract with no way to time out, an async API that can crash). Changing a public header *after* people adopt the library is painful; changing it now is free. This spec locks in those changes, plus smaller behavior fixes, security defaults, and build/CI improvements.

Reading guide: each section says **the problem** in plain words, then **the fix**, then **why this fix and not another**.

---

## Part A — Public API contract fixes (the expensive-later ones)

### A1. Messages must remember tool calls and their ids

**Problem.**
When the model asks for a tool, the request carries an id. Real providers (Anthropic, OpenAI) demand that id back when we send the tool's result, and they also demand that the conversation history still contains the assistant turn *with the tool calls in it*. Our `Message` struct has no place for either — the loop throws both away. So the history we store today literally cannot be replayed to a real API in Phase 1.

**Fix.**
Create one new small header, `include/corvus/types.h`, and move the shared value types into it:

```cpp
struct ToolCall {
    std::string id;         // provider-assigned id
    std::string name;       // tool name
    std::string arguments;  // JSON object string
};

struct Message {
    std::string role;                 // "system" | "user" | "assistant" | "tool"
    std::string content;              // text payload
    std::string name;                 // role=="tool": which tool produced this
    std::string toolCallId;           // role=="tool": pairs result with request
    std::vector<ToolCall> toolCalls;  // role=="assistant": calls it requested
};
```

Loop changes in `src/agent.cpp`:
1. When the model responds with tool calls, **always** append the assistant message (even if its text is empty) and store the tool calls inside it.
2. When a tool finishes, the observation message carries `toolCallId = call.id`.

`memory.h` and `llm_client.h` both include `types.h`. This also removes the circular-include problem that would otherwise appear (`ToolCall` currently lives in `llm_client.h`, which includes `memory.h`).

**Why this fix.**
With this, a Phase 1 client can rebuild the exact wire format either provider wants purely from what memory holds. The alternative — patching ids in later or keeping a side-table — spreads the problem across every backend.

### A2. Tool contract v2 — context in, typed result out

**Problem (two problems, one breaking change).**
1. `Tool::execute(args)` takes only the args string. There is no way to hand a tool a deadline or a cancel signal, and in C++ you cannot kill a running thread — so the design doc's "flagship-critical" per-tool timeout is impossible against this signature.
2. Tools report failure as a flat `"ERROR: ..."` string. The loop can't tell "retry this" from "give up" (open decision 3 in the design doc), and a tool that *legitimately* returns text starting with "ERROR:" would be misread.

Both require changing the same function signature, so we do them together, once, now.

**Fix.**
New types in `types.h`:

```cpp
class CancelToken { /* moves here from agent.h, unchanged */ };

struct ToolContext {
    CancelToken cancel;                                // tool should check this
    std::chrono::steady_clock::time_point deadline{};  // zero = no deadline
    bool expired() const;                              // convenience check
};

struct ToolResult {
    enum class Status { Ok, RetryableError, FatalError, Timeout, Cancelled };
    Status status = Status::Ok;
    std::string content;   // observation text, or the error explanation
    static ToolResult ok(std::string s);
    static ToolResult retryable(std::string why);
    static ToolResult fatal(std::string why);
};
```

New tool contract in `tool.h`:

```cpp
virtual ToolResult execute(const std::string& args, const ToolContext& ctx) = 0;
```

The never-throw rule stays. The enum is what the *loop* branches on; the model still just sees observation text (errors rendered as `"ERROR: <why>"`, same as today).

**Quickstart stays tiny.** `makeTool` gets two forms:
- Simple lambda `std::string(const std::string&)` — auto-wrapped: return value becomes `ok(...)`, a thrown exception becomes `fatal(...)`. Existing examples keep their shape.
- Full lambda `ToolResult(const std::string&, const ToolContext&)` — for tools that want the deadline/cancel signal or to report retryable errors.

**How the per-tool timeout will actually work (Phase 1, but the contract is decided here).**
Honest answer: C++ cannot force-stop a thread, so the design is *cooperative first, watchdog as safety net*:
1. The loop stamps `ctx.deadline` and passes the run's cancel token. Our own built-in tools (HTTP, shell, file I/O) must honor both — their transports get the deadline — so first-party tools never hang.
2. The loop also runs each `execute` under a watchdog (`std::async` + `wait_for`). If the deadline passes: signal cancel, wait a short grace period (~250 ms), then record a `Timeout` observation and move on. A tool that ignores its context leaks a parked thread until it eventually returns — this is documented, and it is the ceiling of what in-process C++ allows.
3. `AgentCallbacks` gains `onToolTimeout(name)` so hosts (e.g. a ROS2 node) can observe it.
4. Genuinely untrusted/risky third-party code belongs behind MCP (Phase 3), where it is process-isolated and *can* be killed. The story is consistent: in-process = cooperative, out-of-process = enforced.

**Why this fix.**
Typed result beats an `"ERROR:RETRY:"` string convention: nothing to parse, no collision with legitimate tool output, and retry/backoff logic branches on an enum instead of string-prefix matching. Doing it in the same breaking window as the context parameter means the `Tool` interface breaks exactly once, before anyone depends on it.

### A3. Agent becomes a safe shared handle, one run at a time

**Problem.**
Three related hazards in the async story (the library's headline feature):
- `runAsync` captures raw `this`; destroy or move the `Agent` while the run is in flight → undefined behavior. A code comment currently "guards" this.
- `Agent` is implicitly copyable; a copy silently shares the same memory/registry, which users won't expect.
- Two overlapping runs on the same agent interleave their turns into one conversation — no crash, just a silently garbled transcript.

**Fix.**
Move everything the loop needs into one internal state block, held by `shared_ptr`:

```cpp
class Agent {
public:
    RunResult run(const std::string& task, const AgentCallbacks& cb = {});
    std::future<RunResult> runAsync(const std::string& task, CancelToken token = {},
                                    AgentCallbacks cb = {});
private:
    struct State {
        LLMClientPtr llm;
        ToolRegistryPtr registry;
        MemoryPtr memory;
        Strategy strategy;
        int maxIterations;
        std::atomic<bool> running{false};
    };
    std::shared_ptr<State> state_;   // the only data member
};
```

- The `runAsync` lambda captures `state_` **by value**. The future itself keeps the state alive, so destroying or moving the `Agent` mid-run is now structurally safe — no comment needed.
- Copying an `Agent` is now *defined* behavior with a name: "copies are handles to the same agent and the same conversation." What was a trap becomes the documented semantic.
- Overlap guard: at run start, `state->running.exchange(true)`; if it was already true, throw `std::logic_error("corvus: this Agent is already running; agents are single-run — build a second Agent for concurrent tasks")`. An RAII guard resets the flag on every exit path. In the async case the exception surfaces at `future.get()`, which is standard C++ behavior.

**Why this fix and not per-run transcript copies.**
Giving each overlapping run its own private copy of memory would "work," but it hides the bug: users who believed they shared a conversation would silently get forked ones. Failing fast with a clear message is the honest behavior; users who want concurrency build two agents (which is cheap).

---

## Part B — Behavior fixes (small, each with a test)

### B4. "Last text on early stop" should be true

**Problem.** `RunResult.output`'s header comment promises the last assistant text when the run stops early, but the code only ever sets the `"[stopped: reached maxIterations]"` sentinel.

**Fix.** Whenever the loop appends non-empty assistant text, also set `result.output` to it. On hitting the iteration cap, the caller now gets the model's last words; the sentinel appears only when there were none.

### B5. Unimplemented strategies must refuse loudly

**Problem.** `withStrategy(Strategy::ReAct)` is accepted and silently runs the ToolCalling loop instead.

**Fix.** `AgentBuilder::build()` throws: `"corvus: only Strategy::ToolCalling is implemented — ReAct arrives with Phase 1"`. Same fail-fast philosophy as the existing backend stubs, and the builder is already the validation choke point.

### B6. Validate maxIterations

**Problem.** `withMaxIterations(0)` (or negative) builds fine and every run instantly returns "[stopped]".

**Fix.** `build()` throws unless `maxIterations >= 1`.

### B7. Escape all control characters in schema JSON

**Problem.** The JSON escaper handles `" \ \n \t \r` but not other control characters (bytes below 0x20) — a stray `\x01` in a description produces invalid JSON.

**Fix.** Add one branch: any remaining char `< 0x20` is emitted as `\u00XX`. Add a test containing an embedded control character.

---

## Part C — Security defaults

### C1. No silent tool shadowing

**Problem.** Registering a tool with an existing name silently replaces the old one. Once MCP tools arrive (Phase 3), a third-party server exposing a tool named `read_file` would silently replace our jailed built-in — a classic "tool poisoning" setup.

**Fix.**

```cpp
enum class OverwritePolicy { Error, Replace };
void registerTool(ToolPtr tool, OverwritePolicy policy = OverwritePolicy::Error);
```

Default = throw on duplicate name. Misconfiguration now fails at startup, not at exploit time. Deliberate replacement is still one argument away. The existing "re-register overwrites" test flips to asserting the throw; a new test covers explicit `Replace`.

**Phase 3 addendum (recorded here, implemented later):** MCP tools register under a namespaced name `<server>__<tool>` (double underscore — provider tool-name rules allow only `[a-zA-Z0-9_-]`, max 64 chars, so dots are out). Collision with a built-in becomes impossible by construction.

### C2. Validate tool arguments before dispatch (Phase 1 item, decided now)

**Problem.** The model's argument JSON goes straight into `execute()` unchecked. Cloud providers mostly enforce schemas and GBNF constrains local models, but the ReAct fallback parses arguments out of free text — those arrive unvalidated.

**Fix (lands with nlohmann/json in Phase 1).** Before dispatch, the loop checks: arguments parse as a JSON object, all `required` keys are present, and primitive types match the schema. Failure → a `RetryableError` observation so the model can correct itself. Deliberately *not* full JSON Schema validation (a rabbit hole); required-keys + primitive types catches the real failure class. Also: cap argument size (64 KB) as cheap denial-of-service insurance.

---

## Part D — Build & CI

### D1. CMake: behave well as a dependency

**Problem.** `CORVUS_BUILD_TESTS` defaults ON unconditionally — anyone pulling corvus via FetchContent gets our tests in their build. And there are no install/export rules, so `find_package(corvus)` doesn't work at all.

**Fix.**

```cmake
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(CORVUS_IS_TOP_LEVEL ON)
else()
  set(CORVUS_IS_TOP_LEVEL OFF)
endif()
option(CORVUS_BUILD_TESTS    "Build the test suite" ${CORVUS_IS_TOP_LEVEL})
option(CORVUS_BUILD_EXAMPLES "Build the examples"   OFF)
```

Plus standard install/export: `install(TARGETS corvus EXPORT corvusTargets ...)`, install the `include/corvus` headers, and generate `corvusConfig.cmake` + a version file via `CMakePackageConfigHelpers`. Result: both FetchContent *and* `find_package(corvus)` work — which is what "clean CMake integration" in the README actually has to mean.

### D2. CI: ThreadSanitizer, with tests worth running under it

**Problem.** The design doc promises ASan/UBSan/TSan; CI only runs ASan+UBSan. And TSan over a test suite with zero concurrency tests proves nothing.

**Fix.** Add a TSan job (Linux, clang, Debug, `-fsanitize=thread`) *and* new concurrency tests it can bite on: two sequential `runAsync` runs, and an overlap test asserting the `std::logic_error` from A3.

---

## Implementation order

1. `types.h` refactor — foundations shared by A1 and A2.
2. A1 message changes + loop changes.
3. A2 tool contract v2 + `makeTool` overloads.
4. A3 agent state block + running guard.
5. B4–B7 behavior fixes.
6. C1 overwrite policy.
7. D1 CMake, D2 CI + concurrency tests.

Everything stays offline/deterministic (MockLLM). Expected test growth: roughly +8–10 cases.

## Ripple effects on the main design doc (to apply after this spec is approved)

- Open decision 3 (tool error contract) → **resolved: typed `ToolResult`**.
- Tool contract v2 (`ToolContext` parameter) recorded as the stable Phase 0 interface.
- Per-tool timeout mechanism (cooperative + watchdog + documented abandon) written into Phase 1.
- MCP tool namespacing + tool-description-injection mitigations added to Phase 3.
- Memory trimming pulled forward into Phase 1 (first real 50-turn session will hit it).
- SECURITY.md + disclosure policy added to Phase 5/6 deliverables.
- Positioning: prefer "runs inside latency-sensitive processes without blocking them" over raw "low-latency/HFT" claims.
