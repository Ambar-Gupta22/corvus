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

- No code behavior changes. No public signature changes.
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

**Entry template (every file):** Role → Key types/functions table (name · what · why) →
Invariants → Tests that pin it → `file:line` links.

Reading order, bottom of the dependency graph upward:

- **Part 0 — How to read this.**
- **Part 1 — Vocabulary:** `types.h` (`ToolCall`, `Message` wire round-trip, `CancelToken` shared
  atomic flag, `ToolContext::expired()`, `ToolResult` statuses + factories); `strategy.h`.
- **Part 2 — The hands:** `tool.h` (`Tool`, `FunctionTool`, both `makeTool` forms, never-throw
  enforcement); `schema.h`/`schema.cpp` (fluent builder, JSON escaping); `tool_registry.h`/`.cpp`
  (mutex map, `OverwritePolicy`, anti-shadowing rationale).
- **Part 3 — Recall:** `memory.h`/`memory.cpp` (`Memory` seam, `InMemoryMemory`, future policy slot).
- **Part 4 — The brain seam:** `llm_client.h` (`LLMResponse`, `ToolSpec`, `TokenCallback`, factory
  stubs in `clients_stub.cpp`); `mock_llm.h`/`mock_llm.cpp`.
- **Part 5 — The wire (PR1):** `http_transport.h` (`HttpRequest`/`HttpResponse`, `ChunkCallback`
  bool-abort semantics, `post()` + cancel, `defaultHttpTransport()`); `httplib_transport.cpp`;
  `mock_http_transport.h` (`enqueue`/`enqueueStream`).
- **Part 6 — The loop:** `agent.h`/`agent.cpp` (`AgentCallbacks`, `RunResult`, `run` vs `runAsync`,
  `State` shared handle and why the future owns it, `runImpl` walked step by step);
  `agent_builder.h`/`.cpp` (fail-fast validation); `corvus.h` umbrella.
- **Part 7 — End-to-end trace:** `examples/mock_quickstart.cpp` followed call by call through every
  unit, with a Mermaid sequence diagram.
- **Part 8 — Scaffolding:** root/tests/examples CMakeLists (options, FetchContent deps,
  install/export, `corvusConfig.cmake.in`); `ci.yml` (3-OS matrix, ASan/UBSan, TSan);
  `.clang-format`/`.clang-tidy`; tests layout + doctest; recipes: add a tool, add a test.

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
