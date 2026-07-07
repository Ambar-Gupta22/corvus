# Phase 0, explained in plain language

This document walks through **everything** built in Phase 0 of `corvus`, in simple language, with the **reason** behind each choice and the **alternatives** that were rejected. Read it top to bottom — each part builds on the last. Wherever you might want to weigh in, there's a **👉 Your input** note.

> ⚠️ **Update (2026-07-06):** after this was written, Phase 0 was hardened — see [specs/2026-07-06-phase0-hardening-design.md](specs/2026-07-06-phase0-hardening-design.md). The biggest contract changes, in the same plain language:
> - Tools now return a typed **`ToolResult`** (ok / retryable error / fatal error) instead of a bare string, and `execute` receives a **`ToolContext`** (cancel signal + deadline). The 👉 question about "string vs typed result" below is **resolved: typed**. The simple string-lambda `makeTool` form still exists.
> - `Message` now also records **which tool calls the assistant made and their ids**, so the transcript can be replayed to real providers in Phase 1.
> - `Agent` is a safe shared handle: destroying it mid-`runAsync` is fine, and starting two overlapping runs on one agent throws instead of garbling the conversation.
> - Registering two tools with the same name now **throws** unless you explicitly pass `OverwritePolicy::Replace` (stops a rogue tool silently replacing a trusted one).
>
> The narrative below still explains the *reasoning* correctly; where code snippets differ, the headers in `include/corvus/` are the truth.

> **What Phase 0 is:** the skeleton. A working agent *loop* with a tool system, memory, and an offline test harness — but with a *fake* LLM (MockLLM) instead of a real one. Real model backends (Anthropic, OpenAI, local) come in Phase 1.
>
> **Why build the skeleton first:** if the loop, the tool plumbing, and the memory are correct and tested, then plugging in a real model later is a small, safe step. If we wired a real model first, every bug would be tangled up with network calls, API keys, and costs — hard to tell whether the *loop* is wrong or the *model call* is wrong. Separate the two.

---

## 1. The mental model (how the whole thing works)

An LLM (large language model) is just a function: text in → text out. It can't *do* anything — it can't search the web, read a file, or call an API. An **agent** fixes that by putting the LLM in a loop with **tools**:

```
1. Give the model the task + a list of tools it's allowed to use.
2. Model replies with EITHER a final answer OR "call this tool with these arguments".
3. If it asked for a tool: we run the tool, get the result, hand it back to the model.
4. Repeat until the model gives a final answer (or we hit a safety limit).
```

That loop is the heart of `corvus`. Everything else exists to support it: tools are the "hands", memory is the "short-term recall", the LLMClient is the "brain", and the Agent is the loop that ties them together.

Phase 0 builds all of these pieces and the loop — with a fake brain so we can test offline.

---

## 2. The Tool — the agent's "hands"

**File:** [include/corvus/tool.h](../include/corvus/tool.h)

A **Tool** is anything the agent can *do*. A weather lookup, a calculator, a web search — each is a Tool. The interface is deliberately tiny:

```cpp
class Tool {
    virtual std::string name() const = 0;          // what the model calls it
    virtual std::string description() const = 0;    // the model reads this to decide WHEN to use it
    virtual std::string inputSchema() const;        // what arguments it takes (JSON schema)
    virtual std::string execute(const std::string& args) = 0;  // do the work
};
```

**Why so small?** The simpler the interface, the more tools people (you, and the community) will write. Four methods, and only two you *must* fill in. LangChain's huge adoption came partly from "writing a tool is easy" — we copy that.

**Why `execute` takes a string and returns a string?** The model speaks in text/JSON. Arguments arrive as a JSON string; the result goes back as text the model can read. Keeping it `std::string` (not a fancy typed object) means the **public header has no heavy dependencies** — anyone can implement a Tool without pulling in a JSON library. (Internally, in your tool body, you parse the JSON however you like.)

**The one hard rule: a tool must NEVER throw an exception.** On failure it returns a string starting with `ERROR: ...`. Why? Because a thrown exception would crash the whole agent loop. Instead, an error is just another *observation* — the model reads "ERROR: city not found" and can decide to try again with a different city. Errors become part of the conversation, not a crash.

### makeTool — writing a tool in one line

Subclassing `Tool` every time is annoying for simple tools. So there's a shortcut:

```cpp
auto weather = corvus::makeTool(
    "weather",                                  // name
    "Get current weather for a city",           // description
    corvus::schema().str("city", "city name"),  // arguments
    [](const std::string& args) {               // the actual work, as a lambda
        return fetchWeather(args);
    });
```

No class, no boilerplate. `FunctionTool` (the class behind `makeTool`) also **automatically wraps your lambda in a try/catch**, so even if your code throws, it gets turned into an `ERROR: ...` string for you. You get the safety for free.

**Why this matters:** this is the "user-defined tools" path you asked for. Built-in tools, your custom C++ tools, and (later) MCP tools are *all just Tools* — the agent can't tell them apart. One uniform contract, three sources.

👉 **Your input:** Is `std::string` in/out the right contract, or would you prefer a typed result (e.g. a struct with `success` flag + payload)? I chose string for simplicity and zero header dependencies. Trade-off: less structure, but far easier to implement.

---

## 3. The Schema builder — describing a tool's arguments without writing JSON by hand

**Files:** [include/corvus/schema.h](../include/corvus/schema.h), [src/schema.cpp](../src/schema.cpp)

The model needs to know what arguments a tool takes. The standard format is **JSON Schema** — but writing JSON Schema by hand is fiddly and error-prone:

```json
{"type":"object","properties":{"city":{"type":"string","description":"city name"}},"required":["city"]}
```

Nobody wants to type that. So there's a tiny builder:

```cpp
corvus::schema().str("city", "city name").integer("days", "how many", false)
```

That generates the JSON above automatically. `.str` = string field, `.num`/`.integer` = number, `.boolean` = true/false. The last argument (`false`) means "optional".

**Why a builder instead of just writing JSON?** Fewer mistakes, reads like English, and it handles annoying details like escaping quotes and building the "required" list for you.

**Why build the JSON by hand in `schema.cpp` instead of using a JSON library?** To keep Phase 0 **dependency-free**. The whole core compiles with zero external libraries right now. We add a JSON library in Phase 1 when we genuinely need it (for talking to real APIs). Until then, a 60-line string builder does the job. (This was verified working — it correctly produced valid schema and only listed required fields.)

👉 **Your input:** Once we add the JSON library in Phase 1, should I rewrite this to use it (cleaner) or keep the hand-rolled version (zero dependency)? My lean: switch to the library for correctness, since we'll have it anyway.

---

## 4. The ToolRegistry — the agent's "toolbox"

**Files:** [include/corvus/tool_registry.h](../include/corvus/tool_registry.h), [src/tool_registry.cpp](../src/tool_registry.cpp)

The registry is a labeled box that holds all the tools. You put tools in by name; the agent looks them up by name when the model says "call `weather`".

```cpp
void registerTool(ToolPtr tool);   // add a tool
ToolPtr get(const std::string& name) const;  // find one
bool has(const std::string& name) const;
std::vector<ToolPtr> all() const;  // list all (used to tell the model what's available)
```

**Why a separate registry instead of just a list inside the Agent?** Because tools come from three places (built-in, your code, MCP) and we want one place that collects them all. It also means you can build a registry once and share it across several agents.

**Why is it thread-safe (uses a mutex/lock)?** Because the agent can run *asynchronously* on another thread (see section 7). If one thread is reading the toolbox while another adds to it, that's a crash. The lock prevents that. Small cost, big safety.

**Why a `std::map` (sorted by name)?** Lookups by name are what we do constantly, and a map gives clean by-name access. Registering by name also means **a tool with a duplicate name overwrites the old one** — predictable behavior, tested.

---

## 5. Memory — the agent's short-term recall

**Files:** [include/corvus/memory.h](../include/corvus/memory.h), [src/memory.cpp](../src/memory.cpp)

The LLM forgets everything between calls — it has no memory of its own. So *we* have to remember the conversation and send the whole history back every time. That history is what `Memory` holds:

```cpp
struct Message { std::string role; std::string content; std::string name; };
// role is "user" | "assistant" | "tool" | "system"
```

Each turn — the user's task, the model's replies, every tool result — gets appended as a `Message`. When we call the model, we send the full list back so it "remembers".

**Why is `Memory` an interface (with `InMemoryMemory` as one implementation)?** Because different users want different storage:
- **InMemoryMemory** (built now) — keeps everything in RAM. Fast, simple, forgotten when the program exits.
- **SqliteMemory** (Phase 1) — saves to a file on disk, so the agent remembers across restarts.
- Someone could write a Redis version, etc.

By making it an interface, you swap storage with **one line** (`.withMemory(...)`) and nothing else in the code changes. This is the **Strategy pattern** — same idea applied to LLM backends and reasoning too.

👉 **Your input:** Right now memory keeps the *entire* history forever. Real conversations eventually overflow the model's context window. Phase 1+ needs a "trimming" strategy (drop old messages, or summarize them). Do you have a preference — simple "keep last N messages", or smarter "summarize old ones"? This is a genuinely hard problem and worth your thoughts.

---

## 6. The LLMClient — the agent's "brain" (and the abstraction over it)

**File:** [include/corvus/llm_client.h](../include/corvus/llm_client.h)

This is the seam between the agent and whatever model is doing the thinking. It's an interface so that Anthropic, OpenAI, Ollama, and llama.cpp all look identical to the agent:

```cpp
struct ToolCall   { std::string id, name, arguments; };  // model wants to use a tool
struct LLMResponse{ std::string text; std::vector<ToolCall> toolCalls; };  // a model turn
struct ToolSpec   { std::string name, description, parametersJson; };  // tool info sent TO the model

class LLMClient {
    virtual LLMResponse complete(messages, tools, onToken) = 0;
};
```

Read `complete` as: "here's the conversation so far and the tools you're allowed to use — what do you do next?" The answer (`LLMResponse`) is either **text** (a final answer) or a list of **tool calls** (it wants to act first).

**Why model the response as "text OR tool calls"?** This is the modern, correct way — it's called **native tool calling**. The model returns *structured* data saying exactly which tool and which arguments, instead of us trying to parse that intent out of free-form text. The old way (ReAct: writing "Action: weather\nAction Input: ...") is fragile and breaks easily. We design for the good way from day one. (ReAct stays as a fallback for dumb models that don't support tool calling.)

**Why the `onToken` callback?** For **streaming** — showing the model's answer word-by-word as it's generated, instead of waiting for the whole thing. Important for a responsive feel. It's optional; pass nothing and you get the full answer at once.

**Why are `anthropic()`, `openai()`, `ollama()` declared here but not implemented yet?** So the **public API is locked in from the start**. The function signatures people will call are decided now; Phase 1 just fills in the bodies. Right now they throw a clear message ("lands in Phase 1 — use MockLLM"). See [src/clients_stub.cpp](../src/clients_stub.cpp). This means the library *compiles and links today* — you can build against the real API shape before the real backends exist.

---

## 7. The Agent — the loop that ties it all together

**Files:** [include/corvus/agent.h](../include/corvus/agent.h), [src/agent.cpp](../src/agent.cpp) — **this is the heart of Phase 0.**

The Agent owns a brain (LLMClient), a toolbox (ToolRegistry), memory, and a couple of safety limits. Its `run()` does exactly the loop from section 1. Step by step, here's what [src/agent.cpp](../src/agent.cpp) does:

```
1. Build the list of tool descriptions to show the model (from the registry).
2. Add the user's task to memory.
3. Loop, up to maxIterations times:
   a. (safety) If someone cancelled, stop now.
   b. Ask the model: complete(conversation so far, tools).
   c. If the model returned a final answer (no tool calls) -> save it, DONE.
   d. Otherwise, for each tool the model asked for:
        - look it up in the registry
        - run it (or record "ERROR: unknown tool" if not found)
        - add the result to memory as a "tool" message
   e. Go back to step (a) — now the model can see the tool results.
4. If we ran out of iterations without a final answer -> stop with a "reached limit" note.
```

That's the whole engine. A few decisions worth explaining:

**Why `maxIterations` (default 10)?** The **loop guard**. Without it, a confused model could call tools forever, racking up cost and never finishing. The cap guarantees the loop always ends. Tested: when the model keeps asking for tools past the limit, the agent stops and reports `completed = false`.

**Why does an unknown tool become an `ERROR:` message instead of crashing?** Same philosophy as before — errors are observations. The model asked for a tool that doesn't exist; we tell it so, and it can recover. Tested.

**Why return a `RunResult` struct (`output`, `iterations`, `completed`) instead of just a string?** Because the caller often wants to know *more* than the answer: did it actually finish, or hit the limit? How many steps did it take (useful for cost/debugging)? A struct carries that without extra calls.

### Async + cancellation — why this is a big deal

```cpp
std::future<RunResult> runAsync(task, CancelToken token, callbacks);
```

`run()` is **blocking** — it doesn't return until the agent finishes, which could be many seconds. That's fatal for the exact users we're targeting:
- A **ROS2 robot** has a control loop that must keep running — it can't freeze for 5 seconds waiting on the AI.
- A **game** must render 60 frames a second — a frozen frame is a stutter.

So `runAsync()` runs the agent on a **separate thread** and immediately hands back a `std::future` (a "claim ticket" you redeem later for the result). Your main loop keeps going.

**CancelToken** lets you *stop* a running agent — e.g. the player walked away, or the robot's goal changed. It's "cooperative": the agent checks the token at the top of each loop iteration and bails out cleanly if cancelled. Tested: a pre-cancelled token makes the agent stop immediately with `[cancelled]`.

👉 **Your input:** The async version uses `std::async` (simple, standard). For heavy production use (many agents at once), a managed **thread pool** is better — but it's more complex. I deferred it. Agree to keep it simple for now?

### The callbacks — streaming + a window into the loop

`AgentCallbacks` is a bundle of optional hooks: `onToken` (streamed text), `onToolCall` (a tool is about to run), `onToolResult` (a tool finished), `onStep` (one loop iteration happened). All optional — ignore them and nothing changes.

**Why?** Two reasons in one mechanism: (1) **streaming** for responsiveness, and (2) **observability** — you can watch exactly what the agent is doing, which is how you debug and trace it. This is the cheap version of what LangChain charges for with "LangSmith". We get the 80% for free, and skip a whole separate tracing system for now.

---

## 8. AgentBuilder — assembling an agent without a giant constructor

**Files:** [include/corvus/agent_builder.h](../include/corvus/agent_builder.h), [src/agent_builder.cpp](../src/agent_builder.cpp)

Instead of one constructor with seven arguments (easy to get the order wrong), you build an agent by naming each piece:

```cpp
auto agent = AgentBuilder()
    .withModel(...)
    .withTool(...)
    .withMemory(...)
    .withStrategy(Strategy::ToolCalling)
    .build();
```

**Why the builder pattern?** Readability (each line says what it sets), flexibility (skip what you don't need — sensible defaults fill in), and **fail-fast validation**: `build()` checks that you set a model and throws a clear error if not, *before* the agent runs. Tested: building with no model throws.

**Sensible defaults baked into `build()`:** no memory set → use InMemoryMemory; no registry set → make one from the tools you added. So the minimum viable agent is just `.withModel(...).build()`.

This is the **public face** of the whole library — the first thing a new user writes. It's designed to match the 12-line quickstart that gets people hooked.

---

## 9. MockLLM — the fake brain that makes offline testing possible

**Files:** [include/corvus/mock_llm.h](../include/corvus/mock_llm.h), [src/mock_llm.cpp](../src/mock_llm.cpp)

MockLLM is a fake LLMClient. You tell it exactly what to "say", in order:

```cpp
MockLLM mock;
mock.callTool("weather", "{\"city\":\"Paris\"}")   // first turn: ask for the weather tool
    .reply("It's sunny in Paris.");                 // second turn: final answer
```

**Why is this one of the most important pieces in Phase 0?** Because it lets us test the *entire* agent loop with **no internet, no API key, no cost, and no randomness**. A real model gives different answers every time — useless for a test. MockLLM gives the *exact same* answer every run, so a test can assert "the output must equal `done`". This is what makes the test suite reliable and what lets contributors run tests instantly.

This is a real engineering signal: serious frameworks ship a way to test agents deterministically. Most beginners don't think of it.

---

## 10. The Strategy enum — choosing how the agent reasons

**File:** [include/corvus/strategy.h](../include/corvus/strategy.h)

```cpp
enum class Strategy { ToolCalling, ReAct, PlanAndExecute };
```

Three ways an agent can think:
- **ToolCalling** — the modern default (section 6). Used now.
- **ReAct** — the older text-based way. A fallback for models without native tool calling. Built later.
- **PlanAndExecute** — make a full plan first, then execute steps. For complex tasks. Phase 4.

**Why an `enum` and not a text string like `"react"`?** Because a typo in `"reactt"` would be a silent bug discovered at runtime. With an enum, a typo is a **compile error** — caught immediately. Safety through the type system.

---

## 11. The build & quality scaffolding (the "top 1%" signals)

These don't add features, but they're what separate a serious project from a hobby script. Each one is a trust signal to anyone deciding whether to use or star the project.

| File | What it is | Why it matters |
|------|-----------|----------------|
| [CMakeLists.txt](../CMakeLists.txt) | The build recipe. Defines the `corvus::corvus` library. | C++ has no standard package manager. CMake + "FetchContent" is how people add your library in **4 lines**. Low setup friction = adoption. |
| [tests/](../tests/) (doctest) | Automated tests for registry, schema, agent loop. | Proof the code works, and a safety net so future changes don't break old behavior. Uses MockLLM → runs offline. |
| [.github/workflows/ci.yml](../.github/workflows/ci.yml) | Runs the build + tests automatically on Linux, macOS, Windows + a "sanitizer" pass that catches memory bugs. | The green ✅ badge is instant credibility. It also means *I* don't need your old compiler to verify — the cloud does it on every push. |
| [.clang-format](../.clang-format) / [.clang-tidy](../.clang-tidy) | Auto-formatting + automated code-smell checks. | Consistent style and catches bugs before review. Signals professionalism. |
| [LICENSE](../LICENSE) (MIT) | Legal permission to use the code. | MIT = anyone (including companies) can use it freely = far more adoption than restrictive licenses. |
| [README.md](../README.md) | The landing page. | Most people decide whether to star within 10 seconds of reading it. |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | "How to add a tool in 5 minutes." | Turns curious readers into contributors on day one. |
| [examples/mock_quickstart.cpp](../examples/mock_quickstart.cpp) | A runnable demo using MockLLM. | Shows the API works, offline, today. |

👉 **Your input:** The copyright line says "corvus contributors". Want your name on it instead? And the README has a `<you>` placeholder for the GitHub URL — tell me your GitHub username when we push.

---

## 12. The design patterns used (ties back to your original LLD goal)

You started this as a "good LLD (low-level design) project". Here are the classic design patterns actually used in Phase 0 — useful for interviews, and each solves a real problem:

- **Strategy** — `LLMClient`, `Memory`, and `Strategy` are all swappable implementations behind an interface. Swap a part without touching the rest.
- **Builder** — `AgentBuilder` assembles a complex object step by step with validation.
- **Command** — each `Tool` is an action wrapped as an object; the agent invokes them without knowing their internals.
- **Registry** — `ToolRegistry` is a central lookup for tools by name.
- **Observer** — `AgentCallbacks` let outside code watch the loop's events without the loop knowing who's listening.
- **Template Method** — the agent loop is a fixed skeleton; the `Strategy` fills in the specific reasoning step.

Every pattern here exists because something *breaks* without it — not for decoration. That's the right way to use them.

---

## 13. What was deliberately NOT built yet (and why)

Good engineering is also about *not* building things too early ("YAGNI" — You Aren't Gonna Need It). Deferred on purpose:

- **Real model backends** (Anthropic/OpenAI/Ollama/llama.cpp) → Phase 1. Build the loop first, prove it, then plug in the brain.
- **SqliteMemory** (persistent memory) → Phase 1.
- **JSON library + HTTP library** → Phase 1, when we actually call real APIs. Keeps Phase 0 dependency-free.
- **MCP client** → Phase 3.
- **Multi-agent orchestration** → Phase 4. Get one agent rock-solid first.
- **Memory trimming/summarization** → needed once we hit real context limits.
- **Thread pool** for async → only if simple `std::async` proves insufficient.

---

## 14. Build status — verified

The full library now **builds and passes all tests on your machine** with MSVC Build Tools 2026 + CMake 4.3:

```
[doctest] test cases: 12 | 12 passed | 0 failed
[doctest] assertions: 35 | 35 passed | 0 failed
[doctest] Status: SUCCESS!
```

The `mock_quickstart` example also runs and drives a real agent loop end to end (model asks for a tool → tool runs → final answer). CI will re-prove this on Linux/macOS/Windows on every push.

*(Historical note: the original 2016 MinGW g++ 6.3.0 on this machine couldn't build it — it's missing `<mutex>`/`<future>` — which is why we installed MSVC. That's the compiler being 9 years old, not a problem with the code.)*

---

## How to give your input

Go through the **👉 Your input** notes above (sections 2, 3, 5, 7, 8, 11). Tell me your preferences, push back on anything that feels wrong, or ask "why" about anything still unclear. Nothing here is locked — Phase 0 is the foundation, and it's easier to adjust now than after Phase 1 builds on top of it.
