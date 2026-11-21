# Loop guards — design spec

**Date:** 2026-11-21
**Status:** Awaiting approval
**Scope:** `feat/loop-guards` — bounded tool fan-out per turn and per run, a bounded observation
size, and a pre-execution **tool gate** that can deny a call before it runs.
**Out of scope:** per-tool timeout (`feat/per-tool-timeout`), per-call schema validation
(`feat/arg-validation`), the per-tool `ToolGuard` (`feat/toolguard-http`), the Phase 4
`ToolPolicy`/RBAC layer, parallel tool execution (Phase 4).

## Goal

Before the first real client lands, the loop must stay bounded and controllable **whatever the
model sends back**. Today it is neither:

1. **Unbounded fan-out.** `maxIterations` bounds turns, not calls. One response can request 30
   (or 3,000) tool calls, and the loop runs them all
   ([agent.cpp L136](../../src/agent.cpp#L136)). Mocks never do this; real models do, and a
   prompt-injected tool result can make them.
2. **Unbounded observations.** A tool returning 50 MB puts 50 MB into memory and into every later
   request. The memory backstop (memory spec §9) recovers from the overflow, but only after a
   failed request, and `ToolGuard` output caps only protect built-in tools.
3. **No veto.** `onToolCall` observes; nothing can stop a call. For corvus's targets this is the
   missing primitive: a robot must be able to refuse `move_to(x=40)` outside its geofence, a game
   must be able to refuse an action the player hasn't confirmed, a CLI must be able to ask a human
   first.

These change public API (`AgentBuilder`, `RunResult`, `Agent`), so they land before the clients
are built on top of it.

Target usage:

```cpp
auto agent = corvus::AgentBuilder()
    .withModel(llm)
    .withTool(moveTo)
    .withToolGate([&](const corvus::ToolCall& call) {
        if (call.name == "move_to" && !geofence.allows(call.arguments))
            return corvus::ToolDecision::deny("target is outside the geofence");
        return corvus::ToolDecision::allow();
    })
    .withMaxToolCallsPerTurn(8)      // defaults are on; these just tighten them
    .build();
```

## Decisions

1. **Secure by default.** All three limits are on with sane defaults; a user can raise them but
   never has to remember to set them. Defaults:

   | Limit | Default | Why this number |
   |---|---|---|
   | `maxToolCallsPerTurn` | 16 | Well above what models emit for legitimate parallel calls (typically 1–5). |
   | `maxToolCallsPerRun` | 64 | A cost/blast-radius budget independent of `maxIterations`, which users raise for long tasks. |
   | `maxObservationBytes` | 64 KiB | ~16k tokens: big enough for real tool output, small enough that one result can't dominate the context. Matches the planned 64 KB arg cap. |

   Each must be ≥ 1; `build()` throws `std::runtime_error` otherwise (same rule as
   `maxIterations`). There is no "unlimited" sentinel: pass a large number.

2. **Every call the model requested gets exactly one paired result.** Providers reject a
   transcript with an unanswered tool call, and the memory sanitizer would drop the whole exchange.
   So a denied, skipped or unknown call is never dropped silently; it gets an error observation the
   model can read and react to:

   | Case | Observation (model-facing) | `ToolResult` status |
   |---|---|---|
   | Over the per-turn cap | `ERROR: skipped: per-turn tool call limit (16) reached` | `FatalError` |
   | Over the per-run cap | `ERROR: skipped: per-run tool call limit (64) reached` | `FatalError` |
   | Unknown tool (unchanged) | `ERROR: unknown tool 'x'` | `FatalError` |
   | Denied by the gate | `ERROR: denied: <reason>` (or `ERROR: denied` if empty) | `FatalError` |
   | Gate threw | `ERROR: denied: tool gate failed` | `FatalError` |

   No new `ToolResult::Status` value: `FatalError` already means "don't retry this call as-is",
   and the enum is public. The model-facing text distinguishes the cases; `RunResult` counts them
   (decision 7).

3. **Order of checks per call:** per-turn cap → per-run cap → registry lookup → gate → execute →
   observation cap. Caps come first because they cost nothing and bound everything after them. The
   gate sees only calls that would otherwise run, against a tool that exists. When
   `feat/arg-validation` lands, schema validation slots in between lookup and gate, so the gate
   only ever sees well-formed arguments.

4. **Counting.** The per-turn cap counts call *slots*: the first N calls of a response are
   processed, the rest are skipped. The per-run budget counts every processed call, including
   unknown and denied ones. A model that keeps hammering a denied tool spends its budget; that is
   intended. When the run budget is exhausted the loop does **not** abort: later calls are
   skipped with the error above, which tells the model to answer with what it has, and
   `maxIterations` still bounds the run. `RunResult.completed` stays the existing signal.

5. **The gate is agent config (builder), not a per-run callback.** `AgentCallbacks` is passed
   per run, so a safety check there is one forgotten argument away from not running. Two
   overloads, mirroring `makeTool`:
   - `withToolGate(std::function<ToolDecision(const ToolCall&)>)`: the common case.
   - `withToolGate(std::function<ToolDecision(const ToolCall&, const CancelToken&)>)`: for gates
     that block (human approval, a confirmation dialog, a remote policy service). They must watch
     the token, or a cancelled run waits on them forever. Cancel during a gate: the gate returns
     whatever it returns; the loop's existing iteration-start check then stops the run.

   One gate per agent; a second `withToolGate` replaces the first (compose in user code if you
   need several). The gate runs on the run's thread (the worker thread for `runAsync`), so like
   callbacks it must be thread-safe if shared between agents.

6. **The gate fails closed.** If the gate throws, the call is denied, not run. The exception
   text is **not** forwarded to the model (it may contain internal detail); the observation is the
   fixed `ERROR: denied: tool gate failed`. The deny `reason` *is* forwarded verbatim, so gate
   authors must not put secrets in it.

7. **Observable.** `RunResult` gains four counters, all additive:
   `toolCallsExecuted`, `toolCallsDenied`, `toolCallsSkipped`, `observationsTruncated`.
   Callbacks: `onToolCall` fires for **every** requested call, before any check (it is the
   "model asked for this" event); `onToolResult` fires for every call with the final
   observation, including skipped and denied ones, so an observer always sees one result per
   call. No new callback (YAGNI).

8. **Observation cap: head + tail, UTF-8 safe.** An over-cap observation keeps its beginning and
   its end with a marker in between, because errors and conclusions usually sit at the end of tool
   output (same reasoning and marker format as memory spec §8.3):
   `<head>\n[...truncated 48213 bytes...]\n<tail>`. The result, marker included, is at most
   `maxObservationBytes`. Cut points move back to a UTF-8 code-point boundary, so truncation never
   produces invalid UTF-8 (the clients' JSON serializer would reject it). The cap applies to the
   rendered observation, so `onToolResult` and memory see exactly what the model sees. If the cap
   is too small to hold the marker plus any content, the observation is the marker alone.

9. **`Agent` constructor stays source-compatible.** The current 5-argument constructor is kept
   (it delegates). A new one takes `AgentLimits` + `ToolGate`:

   ```cpp
   struct AgentLimits {
       int maxIterations = 10;
       int maxToolCallsPerTurn = 16;
       int maxToolCallsPerRun = 64;
       std::size_t maxObservationBytes = 64 * 1024;
   };
   ```

   Grouping the limits stops the constructor growing by one parameter per future limit.

## Relation to `ToolPolicy`, `ToolGuard`, per-tool timeout

| Mechanism | Question it answers | Granularity | Lands |
|---|---|---|---|
| **Tool gate** (this spec) | "May *this agent* run *this call, with these args*, right now?" | per agent, per call, dynamic | now |
| `ToolGuard` (decision 9) | "Is this call itself safe?" (path jail, SSRF block, output cap) | per tool, inside `execute()` | Phase 1 |
| `ToolPolicy` (decision 8) | "May this agent use this tool at all?" (allowlists, scopes, audit) | per agent, static | Phase 4 |

The gate is the execution-time denial primitive that decision 8 anticipated for Phase 4, landing
early. `ToolPolicy` should be built **on** it (a policy object that produces a gate), not beside
it, so there is one denial path in the loop. Gate time does not count against a tool's deadline:
the per-tool timeout stamps `ctx.deadline` only for `execute()`.

## Changes

| File | Change |
|---|---|
| `include/corvus/agent.h` | `ToolDecision` (`allow()`, `deny(reason)`), `ToolGate`, `AgentLimits`; `RunResult` counters; second `Agent` constructor; `State` holds limits + gate. Comment updates for `onToolCall`/`onToolResult`. |
| `include/corvus/agent_builder.h` + `src/agent_builder.cpp` | `withToolGate` (2 overloads), `withMaxToolCallsPerTurn`, `withMaxToolCallsPerRun`, `withMaxObservationBytes`; validation. |
| `src/agent.cpp` | Check order (decision 3), counters, head+tail truncation helper, fail-closed gate. |
| `include/corvus/mock_llm.h` + `src/mock_llm.cpp` | Additive `callTools(std::vector<std::pair<std::string, std::string>>)` to queue one turn with several calls. Tests need it; no other change. |
| `docs/CODE_TOUR.md`, `docs/HISTORY.md`, `CLAUDE.md` | Entries for the above; Appendix A rows for the two loop-guard gaps marked fixed; decision 8 note that `ToolPolicy` builds on the gate. |

## Testing (offline, MockLLM)

- **Per-turn cap:** a turn with 20 calls and cap 16 → 16 executed, 4 skipped; all 20 tool
  messages in memory, paired by id, in request order; `toolCallsSkipped == 4`.
- **Per-run cap:** cap 3 across two turns of 2 calls → 3 executed, 1 skipped; the run continues
  to the final answer.
- **Gate allow / deny:** denied tool never executes (side-effect counter stays 0); observation is
  `ERROR: denied: <reason>`; empty reason → `ERROR: denied`; `toolCallsDenied` counted.
- **Gate fails closed:** throwing gate → not executed, `ERROR: denied: tool gate failed`, exception
  text absent from the observation.
- **Gate sees only real calls:** unknown tool → `unknown tool` error, gate not invoked; skipped
  call → gate not invoked.
- **Cancel-aware gate:** a gate that waits on the token returns once `runAsync`'s token fires; the
  run ends `completed == false`.
- **Callbacks:** `onToolCall` and `onToolResult` each fire once per requested call, including
  skipped and denied ones.
- **Truncation:** over-cap output → length ≤ cap, starts with the head, ends with the tail,
  contains the marker with the right byte count; multi-byte UTF-8 straddling a cut point → output
  is valid UTF-8; a cap smaller than the marker → marker only; under-cap output unchanged.
- **Builder validation:** each limit < 1 throws; defaults applied when unset; second
  `withToolGate` replaces the first.
- **Compatibility:** existing 5-arg `Agent` constructor still builds and uses the default limits.
