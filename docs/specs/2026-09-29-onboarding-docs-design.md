# Onboarding & architecture docs — design

**Date:** 2026-09-29
**Status:** Approved (brainstorming), pending implementation plan
**Branch:** `docs/onboarding-guide` (one PR)

## Problem

The repo has ~2k lines of C++ across 14 public headers, 8 sources, 5 test files, and 1 example, plus
7 docs written at different times (4 specs, 1 plan, 2 explainers). There is no single "start here"
path. A new contributor (or the maintainer returning after a break) must reconstruct the picture from
phase-scoped narratives, some of which predate the Phase 0 hardening and are now partly stale
(e.g. old tool contract, old test counts).

## Goals

1. One entry point that tells any reader what to read, in what order, for their goal.
2. In-depth coverage: what each file does, what each function does and why, which design patterns
   are used and what problem each solves, why the layout is the way it is.
3. A step-by-step history of how the codebase got here (Phase 0 → hardening → PR1), tied to commits.
4. Docs that stay correct as the code changes — detail lives where it will actually be updated.

## Non-goals

- No code behavior changes. No public signature changes. Sharp edges found while writing are
  documented and filed as follow-up issues, not fixed here.
- No new doc tooling (Doxygen site, mkdocs). Plain GitHub-rendered Markdown + Mermaid.
- Not rewriting the specs/plans — they stay as the decision record; new docs link to them.

## Deliverables

```
docs/
  README.md          NEW  index + reading paths
  ARCHITECTURE.md    NEW  the "why": big picture, patterns, invariants, decisions
  CODE_TOUR.md       NEW  the "where": file-by-file, function-by-function, reading order
  HISTORY.md         NEW  the "when": eras, step by step, with commit hashes
  history/
    phase-0-explained.md              MOVED (git mv) + staleness banner
    pr1-http-transport-explained.md   MOVED (git mv) + staleness banner
  specs/, plans/     unchanged (plus this spec)
include/corvus/*.h   comment-only: /// doc-comments on every public declaration
README.md, CONTRIBUTING.md, CLAUDE.md, docs/specs/2026-07-04-memory-design.md
                     link fixes + maintenance rule
```

Layering principle: each doc answers exactly one question — *why* (ARCHITECTURE), *where*
(CODE_TOUR), *when* (HISTORY). Precise per-function contracts (params, throws, thread-safety) live in
header doc-comments beside the code; CODE_TOUR explains each function's *role and rationale* and
links to `file:line` rather than restating the contract.

## 1. ARCHITECTURE.md

1. **What corvus is & who it serves** — positioning (local-first, embeddable, MCP-native), the three
   differentiators, target domains (ROS2, games, edge, HFT).
2. **Component map** — Mermaid diagram of Agent, AgentBuilder, LLMClient (+ MockLLM, factory stubs),
   HttpTransport (+ httplib impl, MockHttpTransport), ToolRegistry/Tool/FunctionTool/Schema, Memory,
   and the value types, showing ownership and dependency direction.
3. **Layout rationale** — why `include/corvus/` is a stable, dependency-light contract; why
   src/tests/examples split; the CMake target `corvus::corvus`, install/export, top-level-only tests.
4. **Design patterns table** — pattern · where · concrete problem it solves here: Strategy
   (LLMClient/Memory/Strategy enum), Builder (AgentBuilder), Command (Tool), Registry (ToolRegistry),
   Observer (AgentCallbacks), Template Method (agent loop), Seam/Test Double (HttpTransport,
   MockLLM, MockHttpTransport).
5. **Core invariants** — tools never throw; one run at a time per Agent (overlap → `logic_error`);
   the future owns run state (destroy/move-safe); registry rejects duplicate names by default;
   tests are offline and deterministic; public headers stay dependency-light.
6. **Decisions** — settled vs parked, one line each, linking to CLAUDE.md / specs (no duplication).
7. **Where the roadmap plugs in** — which seam each phase extends (P1 clients → LLMClient +
   HttpTransport; P1/P2 memory policies → Memory; P3 MCP → Tool/ToolRegistry; P4 → above registry).

## 2. CODE_TOUR.md

The largest doc, and the one a reader keeps open beside the code. Its job: after reading it, a
contributor can explain what every file, type, and function does, why it is shaped that way, what
must never break, and which test would catch it breaking.

### 2.1 Entry template (applied to every file)

```
### <path>  (<N> lines · header|source|test|build · since <era>)
**Role** — one paragraph: what this file owns and what it deliberately does NOT own.
**Depends on / depended on by** — include edges, so the reader sees its place in the graph.
**Walkthrough** — table: Symbol · Kind · What it does · Why it is this way · file:line
**Invariants** — bullet list of rules that must hold (and who enforces each: compiler, runtime
  check, test, or convention only).
**Sharp edges** — surprising behavior a contributor would trip on; each tagged
  (intended | known gap → phase X | bug candidate).
**Tests that pin it** — TEST_CASE names that would fail if the invariants broke.
**Change checklist** — what else must change if you edit this file.
```

"Why" is the most important column. Every row answers it with the concrete problem the choice
solves in this codebase, not a generic textbook description.

### 2.2 Structure

**Part 0 — How to read this doc.** Reading order rationale (bottom of the include graph up), the
entry template, a Mermaid include-dependency graph of `include/corvus/*.h`, and a glossary: turn,
iteration, observation, tool spec, seam, test double, handle.

**Part 1 — Vocabulary (`types.h`, `strategy.h`)**
- Why one shared value-type header: `tool.h`, `memory.h`, `llm_client.h` must never include each
  other; `types.h` is the leaf that breaks the cycle.
- `ToolCall {id, name, arguments}`: `id` is provider-assigned and must be echoed back;
  `arguments` stays a raw JSON string (the core does not parse JSON — YAGNI until arg-validation).
- `Message {role, content, name, toolCallId, toolCalls}`: which fields matter per role, and why an
  assistant turn must keep its `toolCalls` and a tool turn its `toolCallId` (provider wire formats
  reject unpaired tool results). Illustrated with a 4-message transcript table.
- `CancelToken`: `shared_ptr<atomic<bool>>`, so copies share one flag. Explains why copying
  instead of a reference is the thread-safe choice for `runAsync`. Cancellation is cooperative only.
- `ToolContext {cancel, deadline}` + `expired()`: zero time_point means "no deadline". Why C++ cannot
  kill a thread, and therefore why tools must poll.
- `ToolResult`: five statuses, three factories. Sharp edge: `Timeout`/`Cancelled` have no factory
  yet (known gap, per-tool-timeout PR). The status is for the loop; `content` goes to the model.
- `Strategy` enum: three values, only `ToolCalling` accepted today (builder rejects the rest).
  Sharp edge: the builder message says "ReAct arrives with Phase 1", which the roadmap doesn't
  confirm. Flag for reconciliation.

**Part 2 — The hands (`tool.h`, `schema.h`/`.cpp`, `tool_registry.h`/`.cpp`)**
- `Tool` interface: `name`, `description` (the model reads it, so quality decides whether the
  tool gets used), `inputSchema` (default `{}`), `execute`. The never-throw contract and the
  cooperative-cancel contract, and why the loop depends on both.
- `FunctionTool`: the try/catch wrapper that enforces never-throw for lambda authors (`std::exception`
  → `fatal(what())`, anything else → `fatal("unknown exception in tool")`).
- `makeTool` overloads: the full form (`ToolResult(args, ctx)`) vs the simple form
  (`string(args)` wrapped into `ok`). How overload resolution picks between them. Sharp edge:
  in the simple form, a lambda's thrown exception still becomes fatal through `FunctionTool`.
- `Schema`: fluent `str/num/integer/boolean(name, desc, required=true)`, `json()`, and the implicit
  `operator std::string` that lets it drop straight into `makeTool`. The output shape
  (`{type:object, properties, required[]}`) as an annotated JSON example.
- `schema.cpp::escape`: which characters get escaped and why control characters below 0x20 need
  `\u00XX` (otherwise invalid JSON reaches the provider). Sharp edges: flat schemas only (no
  nested objects/arrays/enums); field names aren't de-duplicated; the type string isn't escaped
  (safe, only internal literals). Open decision 4 (move to nlohmann) is linked.
- `OverwritePolicy {Error, Replace}` + `ToolRegistry`: a `std::map` guarded by a `mutex`.
  `registerTool` rejects null and duplicates (anti-shadowing: a malicious or misconfigured tool
  replacing a trusted one by name). `get` returns nullptr on a miss (the loop turns that into an
  "unknown tool" observation). `all()` returns a snapshot copy, name-ordered because it is a
  `std::map`, so tool spec order is deterministic across runs. Why the registry stays "dumb" and
  policy goes in a layer above (decision 8).

**Part 3 — Recall (`memory.h`/`.cpp`)**
- `Memory` seam: `append`, `context` (returns a copy), `clear`. Why the whole history is re-sent
  each turn (the LLM is stateless).
- `InMemoryMemory`: a mutex-guarded vector. `inMemory()` factory.
- Sharp edges: unbounded growth (intended for now → `lastN` in P1); `context()` copies the full
  vector every iteration (O(history) per turn, acceptable until profiling says otherwise); the
  memory is shared across runs of the same Agent, so the conversation carries over between tasks.
- Where trimming policies and `SqliteMemory` will plug in, linked to the memory spec.

**Part 4 — The brain seam (`llm_client.h`, `clients_stub.cpp`, `mock_llm.h`/`.cpp`)**
- `LLMResponse {text, toolCalls}`: empty `toolCalls` means `text` is the final answer. Both can be
  non-empty (a model can narrate before acting).
- `ToolSpec`: the tool's projection handed to the model. `TokenCallback`: the streaming hook.
- `LLMClient::complete(messages, tools, onToken)`: one turn, stateless.
- Factory stubs `anthropic/openai/ollama` throw `runtime_error`. Why they were declared in Phase 0
  (public API stable from day one) and which PR replaces each.
- `MockLLM`: FIFO `deque<LLMResponse>`, chainable `reply` / `callTool` / `replyAndCallTool`.
  `complete` ignores its inputs, pops the front, and emits the whole text as a single `onToken` chunk.
  Sharp edges: the id is `"mock-call-" + queue_.size()` at enqueue time, so ids can repeat if you
  enqueue after consuming; an empty queue returns the sentinel text `"[MockLLM] no queued
  response"` rather than failing the test; it doesn't record what it was sent (there's no
  `requests()` like the transport mock has). Candidate improvement.

**Part 5 — The wire, PR1 (`http_transport.h`, `httplib_transport.cpp`, `mock_http_transport.h`)**
- Why the seam is cut at HTTP bytes: clients own provider JSON, SSE parsing, and error mapping;
  the transport only moves bytes. This makes the whole client stack testable offline.
- `HttpRequest` (url, headers, body, connect and per-read timeouts, why reads get 120 s for
  streaming), `HttpResponse` (`status == 0` means a transport failure and `error` is set; the three
  body cases: non-streamed, streamed 2xx, streamed non-2xx, shown as a table).
- `ChunkCallback` returns bool: `false` aborts the socket (the early-stop path).
- `post()` contract: blocking, never throws, cancel checked before the request and on every buffer.
  Its limit: a completely silent server is bounded only by `readTimeout`.
- `httplib_transport.cpp`, function by function:
  - `splitUrl`: scheme allowlist `http`/`https`; rejects userinfo `@` (connects to a host other than the
    one shown) and control characters.
  - `hasCtl`: CR/LF header-injection guard. Why it's needed: the multimap path bypasses httplib's check.
  - `iequals`: case-insensitive `Content-Type` override.
  - `HttplibTransport::post`: TLS gate (`CORVUS_HAS_TLS`), pre-flight cancel,
    `set_follow_location(false)` (why: no redirect-based SSRF/credential leak),
    `response_handler` to capture status before the body arrives, `content_receiver` routing
    (2xx goes to onChunk; non-2xx is teed into body with a 256 KB cap; non-streamed bodies are capped at 64 MB, then
    overflow), error precedence (cancelled > aborted > overflow > httplib error).
  - `defaultHttpTransport()` factory. The class sits in an anonymous namespace, so httplib never
    leaks into public headers.
- `MockHttpTransport`: `Script`, `enqueue`, `enqueueStream`, the `onBeforeChunk` hook (fires cancel
  mid-stream), `requests()` recording. How `post` mirrors every real-transport rule (payload
  normalization, non-2xx never reaches onChunk, cancel/abort errors). Why it's header-only and
  not thread-safe.
- The "contract test" idea: the same `test_transport_contract.cpp` pins mock and real behavior,
  so the mock can't drift from reality. Real-transport tests never touch the network (they fail
  before connecting).
- Sharp edge: neither transport header is in the `corvus.h` umbrella. Record whether that's
  intentional (it's internal plumbing for clients) or an omission.

**Part 6 — The loop (`agent.h`/`.cpp`, `agent_builder.h`/`.cpp`, `corvus.h`)**
- `AgentCallbacks` (onToken, onToolCall, onToolResult, onStep). `onStep` doubles as the tracing seam.
  Which thread they run on (the caller's thread for `run`, the async worker for `runAsync`), so
  callbacks must be thread-safe in the async case.
- `RunResult {output, iterations, completed}`: what `output` holds in each exit case.
- `Agent` as a cheap handle: copies share `State`. `State` holds llm, registry, memory, strategy,
  maxIterations, and `atomic running`.
- `RunningGuard` (RAII): `exchange(true)` claims the single-run slot and throws `logic_error`
  if it's taken; the destructor releases it on every exit path.
- `renderObservation`: status → model text (`Ok` passes through; `Timeout`/`Cancelled` get a prefix;
  retryable/fatal become `"ERROR: " + content`).
- `run` vs `runAsync`: why `runAsync` copies `state_`, `task`, `token`, and `callbacks` into the
  lambda (the future owns everything, so the Agent can be destroyed mid-run), and why a concurrent
  run's `logic_error` surfaces at `future.get()`.
- `runImpl` walked step by step, with a numbered listing and a Mermaid flowchart:
  1. claim the slot
  2. snapshot the tool specs once
  3. append the user message
  4. loop up to `maxIterations`:
     1. check cancel
     2. call `onStep`
     3. `complete()`
     4. no tool calls: append the final assistant message, return `completed=true`
     5. otherwise: append the assistant turn with its `toolCalls` (even if the text is empty),
        remember the text as the best answer so far, then for each call: `onToolCall`, build the
        `ToolContext`, look the tool up (unknown → fatal), `execute`, render, `onToolResult`,
        append a tool message paired by `toolCallId`
  5. cap reached: `completed=false`, with the last text or the `"[stopped: reached maxIterations]"` sentinel.
- Sharp edges (each tagged with its fixing PR):
  - cancel is checked only at the start of an iteration and is not passed into `complete()`
    (→ anthropic-client PR)
  - `RetryableError` is treated like `Fatal`, with no retry yet (→ retries PR)
  - `ctx.deadline` is never set (→ per-tool-timeout PR)
  - tool calls in one turn run sequentially (→ Phase 4)
  - a cancel returns `"[cancelled]"` even if earlier text existed
  - a tool that ignores ctx blocks the whole run
  - an exception thrown by `llm->complete()` propagates out of `run()` (not caught by the loop)
- `AgentBuilder`: fluent setters, then `build()` validation order: model required, ToolCalling
  only, maxIterations ≥ 1, default memory, default registry, register the `withTool` tools.
  Sharp edges: `withTool` plus a caller-supplied registry mutates the caller's registry; calling
  `build()` twice on one builder throws on duplicate names, because the tools are re-registered.
- `corvus.h`: the umbrella includes and version macros (0.0.1, which must stay in sync with
  `project(VERSION)`; nothing enforces this today).

**Part 7 — End-to-end trace**
`examples/mock_quickstart.cpp` followed call by call:
- `makeTool` → `Schema` → `MockLLM` scripting → `AgentBuilder::build` → `run` → `runImpl`
- iteration 1: tool call → registry → `FunctionTool::execute` → observation → memory
- iteration 2: final text → `RunResult`

The trace includes a Mermaid sequence diagram and a table of the exact memory contents after each
step (role / content / toolCalls / toolCallId). A closing "what changes in Phase 1" note shows the
one-line swap from MockLLM to `anthropic(...)` and which new units (client + transport) then join
the trace.

**Part 8 — Scaffolding and workflows**
- Root `CMakeLists.txt`, section by section:
  - why the minimum is 3.18 (`SOURCE_SUBDIR`)
  - the top-level-only test default
  - the four options (`CORVUS_WITH_LLAMACPP` reserved for later)
  - C++17 with no extensions
  - FetchContent deps pinned by tag and never `add_subdirectory`'d
  - PRIVATE third-party includes (the dependency-light public headers are enforced here)
  - the optional-TLS logic and Windows `ws2_32`/`crypt32`
  - warnings `/W4` / `-Wall -Wextra -Wpedantic`
  - install/export plus `corvus::` namespace, and the version file with SameMajorVersion
- `cmake/corvusConfig.cmake.in`: `find_dependency(Threads)`, and conditional OpenSSL for static
  consumers.
- `tests/CMakeLists.txt`: the doctest-as-interface-target trick (and why: doctest's CMake is
  rejected by CMake 4). `examples/CMakeLists.txt`.
- `ci.yml`: the four-entry build matrix, the ASan+UBSan job, the TSan job (which tests exercise the
  threading), and what each job catches.
- `.clang-format`, `.clang-tidy`, `.gitattributes`, `install.ps1`, `skills-lock.json`: one line each
  on purpose (and whether contributors need to care).
- Test suite map: all 36 `TEST_CASE`s grouped by file (`test_agent` 12, `test_registry` 4,
  `test_schema` 7, `test_transport_contract` 13), each mapped to the invariant it pins.
  `test_main.cpp` is the doctest entry point. The count is re-verified by running the suite.
- Recipes, as numbered steps with code:
  - add a tool (lambda form and subclass form)
  - add a test with MockLLM
  - add an `LLMClient` backend behind `HttpTransport`
  - add a `Memory` policy
  - the PR checklist

**Appendix A — Sharp-edges index.** Every sharp edge from Parts 1–8 in one table: edge ·
file:line · intended / known gap (PR) / bug candidate. The bug candidates found so far are the
`build()`-twice duplicate throw, MockLLM id reuse, the ReAct-phase message mismatch, and the
umbrella/version-sync gaps. They get documented in the tour, not fixed in this PR (no behavior
changes); each one becomes a follow-up issue.

**Appendix B — Design patterns → code locations.** Cross-reference back to ARCHITECTURE §4,
pointing at the exact file:line where each pattern lives.

## 3. HISTORY.md

**Era template:** Goal → What changed (files) → Key decisions + why → Breaking changes → Commits →
Spec link.

1. **Origins** — HTML vision → framework-first reorder (`6aaea50`, design spec 2026-06-29).
2. **Phase 0 foundations** — core loop, tools, memory, MockLLM, CI.
3. **Memory design** — spec 2026-07-04 (composable policies, overflow backstop).
4. **Phase 0 hardening** — spec 2026-07-06: `25a4559` (contracts A1–A3, B4–B7: message round-trip,
   tool contract v2, agent handle semantics), `2ca1970` (registry anti-shadowing, breaking),
   `3bcd4ce` (install/export, test gating), `39e9c10` (TSan CI).
5. **Phase 1 planning** — cloud-clients spec + plan (`a926ae9`, `8bb15ef`), PR workflow (`f35475d`).
6. **PR1 — HTTP transport** (`1882816`, #1), step by step: problem (offline-testable clients) → cut
   at the HTTP boundary → each of the 10 changed files and why → security-review findings + fixes →
   what it unlocks for PR2/PR3.
7. **What's next** — short pointer to the Phase 1 branch table in CLAUDE.md (not duplicated).

Every entry cites commit hashes so `git show <hash>` works as a revision aid.

## 4. docs/README.md (index)

- Reading paths: **New** → ARCHITECTURE → CODE_TOUR → HISTORY. **Revising** → HISTORY → CODE_TOUR
  Parts 6–7. **Adding a feature** → CODE_TOUR Part 8 → relevant spec → CONTRIBUTING.
- Table of every doc: purpose · status (current / historical) · last updated.

## 5. Header doc-comments

`///` comments on every public class, struct, enum, and function in `include/corvus/*.h`: one-line
brief, params, return, throws, thread-safety where relevant. Match existing comment style/density.
Comment-only diff — no signature, include, or behavior change. Enforced by the build + test run
staying green and a diff review showing only comment lines changed.

## 6. Archiving the old explainers

`git mv` both explainers to `docs/history/`. Prepend a banner: *"Historical snapshot (written
<date>). May be stale — current reference is [CODE_TOUR.md](../CODE_TOUR.md)."* Update inbound links
in: `README.md` (lines ~111, 126–127), `CLAUDE.md` (lines ~28, 138),
`docs/specs/2026-07-04-memory-design.md` (line 6). Root README docs table points to the new index.

## 7. Maintenance rule

Added to CONTRIBUTING.md and CLAUDE.md: a PR that changes a public header updates its doc-comments
and the matching CODE_TOUR entry; a merged feature PR adds a HISTORY entry.

## Accuracy

- Every claim is checked against current code, not against older docs.
- Numbers (test cases/assertions) come from an actual build + test run on this branch.
- `file:line` links are generated after the header doc-comment pass (comments shift line numbers).
- Mermaid diagrams must render on GitHub (checked in the PR preview).

## Testing / done criteria

- `cmake --build` + `ctest` pass (proves the comment-only header pass broke nothing).
- No dead relative links (scripted check over `docs/`, `README.md`, `CLAUDE.md`, `CONTRIBUTING.md`).
- Every file in `git ls-files` under `include/`, `src/`, `tests/`, `examples/`, `cmake/`,
  `.github/`, plus the root build/lint configs, has a CODE_TOUR entry.
- CI green on the 3-OS matrix before squash-merge.
