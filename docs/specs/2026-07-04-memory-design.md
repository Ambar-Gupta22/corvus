# Memory management design — corvus

**Date:** 2026-07-04 · **Revised:** 2026-10-08 (v2, see §17)
**Status:** Accepted (decision record)
**Resolves:** open decision #5 in CLAUDE.md (memory trimming) and expands it into a full memory roadmap.
**Related:** [2026-06-29-jarvis-cpp-design.md](2026-06-29-jarvis-cpp-design.md) (main design), [2026-07-15-cloud-clients-design.md](2026-07-15-cloud-clients-design.md) (error contract, system extraction, prompt caching), [phase-0-explained.md](../history/phase-0-explained.md) §5.

## 1. Problem

The LLM is stateless; `Memory` re-sends history every turn. Today `InMemoryMemory` keeps everything forever (unbounded), so any long-running agent eventually overflows the model's context window and the run dies. We need bounded memory — but our two audiences pull in opposite directions:

- **Long-running autonomous loops** (ROS2, game NPC, HFT, embedded): history grows for hours; every turn must stay cheap, deterministic, and free of surprise LLM calls or heavy dependencies.
- **Conversational assistants** (the post-1.0 Jarvis demo): users expect "remember what we said" and eventually "remember facts across sessions"; an extra LLM call for summarization is acceptable.

One fixed design cannot serve both. The design must let each user pay only for what they need.

Beyond "don't overflow", production memory has three more jobs the v1 design under-specified: **never corrupt the transcript** (a half-written exchange poisons every later call — permanently, once persisted), **stay cache-friendly** (provider prompt caching only pays off when the request prefix is stable), and **be observable** (silently dropped context is an invisible bug).

## 2. Decision summary

**Memory is not a fixed tier hierarchy. It is a set of independent, composable policies behind the existing `Memory` interface.** The original 3-tier sketch (short-term / summary / long-term) survives as three *capabilities*, but they compose differently:

| Capability | Mechanism | Cost profile | Phase |
|---|---|---|---|
| Unbounded history | `InMemoryMemory` (today) | zero; overflows eventually | 0 ✅ |
| Atomic exchange commits | `Memory::appendAll` + loop buffers the in-flight iteration | zero | 1 (with the first real client) |
| System prompt as agent config | `AgentBuilder::withSystemPrompt` — never stored in memory | zero | 1 (with the first real client) |
| Persistence | `SqliteMemory` — sessions, versioned schema, transactional writes, load-time repair | zero extra per turn | 1 |
| Short-term bound — count | view policy `lastN(k)`, batched (cache-friendly) | zero-dep, deterministic | 1 |
| Transcript sanitizer | pure pass on every outgoing request | zero-dep | 1 |
| Overflow backstop | reactive, request-level, learns a cap | only fires on `ContextOverflow` | 1 |
| Trim observability | `AgentCallbacks::onContextTrim` + `RunResult` counters | zero | 1 |
| Short-term bound — tokens | view policy `maxTokens(t)` + `TokenCounter`, watermarked | needs tokenizer/estimator | 2 |
| Derived budget | `autoWindow()` (budget from model metadata) | exact local, best-effort cloud | 2 |
| Rolling summary | `SummarizingMemory` decorator (holds an `LLMClient`) | +1 LLM call when it fires | 3+ (opt-in) |
| Long-term facts | `FactStore` — separate interface, NOT a `Memory` | embeddings/vector store, opt-in | post-1.0 (with Jarvis demo) |

Core rules:

1. **The `Memory` seam changes additively only.** `append` / `context` / `clear` stay ([memory.h](../../include/corvus/memory.h)); v2 adds one method, `appendAll` (§6), with a default implementation so existing `Memory` subclasses keep compiling. Everything else is implementations, policies, and decorators.
2. **The base path stays pure.** Plain stores (`InMemoryMemory`, `SqliteMemory`) and view policies never do I/O to a model and never require a JSON/HTTP/embedding dependency in a public header. Only the explicitly opt-in `SummarizingMemory` may call an LLM.
3. **Nothing is removed as options are added.** Unbounded stays the default; bounds are opt-in. The menu grows: P0 unbounded → P1 +lastN → P2 +maxTokens/autoWindow → P3+ +summary.
4. **Store ≠ view.** What memory *keeps* (retention) and what the agent *sends* (the view) are separate decisions (§4). Trimming shapes the view; it never deletes from a durable store.
5. **Long-term memory is not on the `context()` path.** Retrieval is query-driven and dependency-heavy; it gets its own interface (`FactStore`), surfaced beside memory (likely as a `recall_facts` tool), never baked into the decorator chain.

## 3. What each bound actually guarantees (be honest in docs)

- **unbounded** — guarantees nothing; will overflow eventually. Right for short tasks.
- **`lastN(k)`** — bounds *growth* (no infinite accumulation). Does **not** guarantee fit: k fat messages (large tool outputs) can still exceed the window. It is a cheap safety valve, not a correctness guarantee. Document this loudly.
- **`maxTokens(t)`** — bounds *size*; guarantees fit **to the budget t**, subject to the edge cases in §8. This is the real fix; `lastN` is the dependency-free stopgap that ships first.
- **backstop** — guarantees the run never dies *silently or corruptly* on overflow: it recovers, or fails with a typed, actionable error and an intact transcript.

Public docs must state: long-running agents **must** set a bound; short chats need not.

## 4. Ownership and layering — who holds what

### 4.1 The system prompt is agent configuration, not memory

`AgentBuilder::withSystemPrompt(std::string)` stores the prompt on the agent. The loop prepends it as a `role == "system"` message to **every outgoing request**; it is never appended to memory. Consequences:

- `clear()` cannot wipe it; trim policies never need to special-case it; persisted sessions never carry a stale prompt (change the prompt, restart, and the new one applies to the old session).
- The client contract is unchanged: a leading `role == "system"` message becomes Anthropic's top-level `system` parameter and stays a system message for OpenAI ([cloud spec](2026-07-15-cloud-clients-design.md)).
- **Compatibility:** a `role == "system"` message a user appended to memory by hand is *pinned* by every policy (never trimmed). Supported, not recommended — `withSystemPrompt` is the documented path.

### 4.2 Store (retention) vs view (window)

- **Store** = the `Memory` implementation. Holds what was appended, subject to its own retention.
- **View** = what the agent sends this request. Computed by a **view policy** — a pure function `select(history) -> {messages, droppedCount, truncatedCount}` — held by the agent, configured through the builder's memory handle (§13).
- Retention per store:
  - `SqliteMemory`: keeps everything by default (disk is cheap, history is an audit trail and future summarizer input); optional `.retain(n)` prunes oldest completed exchanges.
  - `InMemoryMemory` under a bounding policy: discards messages that have **permanently** left the view, so RAM stays bounded on hours-long loops. Unbounded `InMemoryMemory` keeps everything (today's behavior).
- Policy cut points are computed on **monotonic message sequence numbers**, so a store discarding old messages never shifts the window. (Exact accessor — e.g. a first-sequence offset on the store — is a plan detail.)

### 4.3 Request assembly (one place, in the loop)

```
request = [system prompt]                         // §4.1, if set
        + sanitize( view( memory.context() ) )    // §5, §7
        + in-flight messages not yet committed    // §6 — the current task on iteration 1
```

The backstop (§9) operates on this assembled **request**, never on the store.

## 5. Transcript sanitizer (always on, every request)

A pure, dependency-free pass over the viewed history before it is sent. Defense in depth — the loop should never produce these shapes, but hand-built memories, older persisted sessions, and bugs can:

1. Drop an assistant turn whose `toolCalls` are not **all** answered by following `role == "tool"` messages (match by `toolCallId`), together with its partial results — an incomplete exchange is removed as a unit.
2. Drop `role == "tool"` messages whose `toolCallId` has no preceding request.
3. Never reorder, rewrite content, or invent messages.

Every drop is reported through `onContextTrim` (§11) with source `Sanitizer`. **Merging consecutive same-role turns is the client's job** (wire format), not the sanitizer's: e.g. Anthropic tool results (a user-role turn) followed by a new user task are merged into one user turn by `AnthropicClient`.

## 6. Atomic exchange commits (always on)

Today the loop appends the user task *before* calling the model ([agent.cpp](../../src/agent.cpp)). Once `complete()` can throw (`LLMError`, cloud spec), a failure leaves an unanswered user turn; the next run adds a second one. A `Tool` subclass that breaks the never-throw rule leaves `toolCalls` with no results. With `SqliteMemory` that corruption is permanent.

Rule: **memory only ever receives complete exchanges.**

- **Exchange** = one model call's outcome: the assistant turn (text and/or `toolCalls`) plus every paired tool result — preceded, on the run's first iteration, by the user task.
- The loop buffers the in-flight exchange locally and commits it with one `memory->appendAll(batch)` call once complete (final answer received, or every tool result in hand).
- If the model call throws, the run is cancelled mid-request, or a tool escapes the contract, the uncommitted buffer is discarded. A run that fails on its first call leaves memory **exactly as it was** — the caller can retry the same task cleanly.
- Cancel between iterations keeps all previously committed exchanges (each is valid on its own).
- `Memory::appendAll(const std::vector<Message>&)`: new virtual; default implementation loops `append`. `InMemoryMemory` overrides it under one lock; `SqliteMemory` overrides it as one transaction.

## 7. View rules (apply to every policy)

Trimming only ever drops **old, completed** exchanges, oldest first. It must never drop:

1. **Pinned messages** — any stored `role == "system"` message (compat, §4.1).
2. **The current exchange in flight** — defined structurally as the most recent user task (latest `role == "user"` message) and everything after it. Computable from the message list alone, so policies stay pure functions.
3. **Half of a tool exchange.** An assistant-with-`toolCalls` message and all its paired tool results are one atomic unit — drop all or none. Providers reject orphaned tool results.

Because 2 and 3 are structural, a policy configured "too small" cannot produce an invalid request — the view simply exceeds the soft target to stay valid. The builder validates only nonsense (`k < 1`, negative slack).

**Batched trimming (cache rule).** Providers cache the request *prefix* (Anthropic `cache_control` breakpoints, OpenAI automatic caching); a cache hit costs roughly a tenth of a normal input token. A strict sliding window changes the prefix on every request, so every call pays full price for the whole history — on long agent loops a several-fold cost and latency penalty. Every count/token policy therefore cuts in **jumps**: when the view exceeds its high-water mark, it cuts back to the low-water mark, then stays put until the high mark is hit again. The cut point is monotonic. Strict sliding is available but opt-in and documented as cache-hostile.

## 8. Count and token policies

### 8.1 `lastN(k)` (Phase 1)

`lastN(k)` keeps at least `k` recent messages; the view grows to `2k − 1` before cutting back to `k` (default slack = `k`), so the cached prefix survives about `k` appends. `lastN(k).slack(0)` = strict sliding (cache-hostile). Cut points snap forward to the next exchange boundary (§7, rule 3).

### 8.2 `TokenCounter` is pluggable, per-backend (Phase 2)

Tokenization is per model family; there is no universal tokenizer, and we do not implement one:

- **`CountEstimator`** (default, universal, dep-free): heuristic ≈ chars/4. Used for trim decisions when nothing better exists.
- **`ExactCounter`** (per backend, when available): llama.cpp exposes `llama_tokenize()` for the loaded GGUF — exact, offline, free (this is why token budgeting lands in Phase 2 with local backends). OpenAI has tiktoken ports (extra dep, opt-in). Anthropic has no offline tokenizer — estimator only; the API's returned `usage` gives exact numbers *post-hoc* and feeds `RunResult` cost reporting, not the trim decision.

`LLMClient` grows two optional capability hooks (default: "unknown"): `contextWindow()` and `tokenizer()`. Backends that know, report; backends that don't leave the estimator in charge.

### 8.3 `maxTokens(t)` — don't trust the raw number (Phase 2)

Watermarked like `lastN`: exceed the budget → cut back to ~75% of it (low-water ratio tunable). Three edge cases break a naive budget:

1. **Single oversized message > whole budget** (giant tool output). Trimming count can never fix this; the policy truncates that message **head + tail** with a size marker — `[...truncated 48213 bytes...]` — because errors and conclusions usually sit at the end of tool output. Prevention lives at the source too: the `ToolGuard` **output cap** (main design doc) truncates huge tool results before they enter memory. Same problem attacked from both ends.
2. **User budget > model window.** Clamp: `effective = min(t, window − reserve)`; builder warns loudly at build time when it can see the mismatch ("maxTokens 8000 exceeds model window 4096").
3. **No output headroom.** The window is input + output combined; packing it full leaves the model no room to answer. Reserve `maxOutputTokens + safetyMargin` before filling.

`autoWindow()` is the smart default that sidesteps all three: derive the budget from the model (`window − maxOutput − margin`) instead of trusting a hand-entered number. Exact for local backends (GGUF metadata); config-table/best-effort for cloud.

## 9. Overflow backstop (Phase 1, always on)

Proactive policies reduce overflow; they cannot eliminate it (model swaps, estimator error, unbounded chosen). The **agent loop** (which owns request assembly) handles `LLMError` with `Kind::ContextOverflow` (cloud spec) reactively, as a fixed sequence — not a user choice:

1. **Truncate** oversized tool results in the assembled request (head + tail, §8.3). In Phase 1, with no token counter, "oversized" = larger than a fixed fraction of the failed request's size (constants are internal and tunable).
2. **Trim harder**: drop old exchanges from the request (respecting §7) down to a fraction of the failed request's size.
3. **Retry** — bounded (default 2 attempts). The client's `RetryPolicy` never retries `ContextOverflow` itself; only the backstop does, because only it can change the request.
4. **Still failing** → rethrow `LLMError{ContextOverflow}` with an actionable message ("history exceeds the model window even after trimming to N messages — set a memory bound or shrink tool outputs"). Per the cloud error contract this propagates from `run()` / rethrows at `future.get()`. The transcript is intact (§6) — never a crash, never a silently half-written run.

**Learned cap.** After a recovery, the agent remembers ~80% of the size of the request that overflowed (chars in Phase 1; tokens once a `TokenCounter` exists) and pre-trims later requests to that cap for its lifetime — the model is fixed per agent, so the same overflow would otherwise recur and cost a failed call every turn.

The backstop **never mutates the store** — it shapes the outgoing request only. Belt (policy, proactive) + suspenders (backstop, reactive). The backstop exists under **all** policies including unbounded.

## 10. Persistence — `SqliteMemory` (Phase 1)

Industry-grade persistence decisions, made now because each is a breaking schema change later:

- **Sessions.** `sqlite(path, sessionId)`; omitted id = `"default"`. One database file holds many independent conversations (the equivalent of LangGraph's `thread_id`). `clear()` clears only its own session.
- **Versioned schema.** A `meta` table carries `schema_version`; opening runs forward migrations in a transaction; opening a database from a *newer* corvus fails loudly instead of misreading it.
- **Forward-compatible rows.** Each message is stored as a JSON document (the full `Message`) plus indexed columns (`session_id`, `seq`, `role`). New `Message` fields — images, provider thinking blocks that must round-trip verbatim — need no column migration.
- **Transactional writes.** `appendAll` = one transaction (§6). WAL mode for concurrent readers.
- **Load-time repair.** Opening a session runs the §5 sanitizer over stored history; anything dropped is logged once, not silently ignored.
- **Retention.** Keep-all by default; `.retain(n)` optional (§4.2).
- **Threading.** Same thread-safety guarantees as `InMemoryMemory`; SQLite is compiled in serialized mode or guarded internally.

## 11. Observability

- `AgentCallbacks::onContextTrim(const TrimEvent&)` — fires when the view's cut point moves, when the sanitizer drops anything, and on each backstop step. `TrimEvent { source: Policy | Sanitizer | Backstop; droppedMessages; truncatedMessages; }`.
- `RunResult` gains `trimmedMessages` and `truncatedMessages` (summed over the run) and `backstopRecoveries`, so a dashboard can alert on "this agent is losing context".
- Tracing hooks report counts and reasons, never message content (content may be sensitive).

## 12. Rolling summary — `SummarizingMemory` (Phase 3+, opt-in)

Decorator: is-a `Memory`, has-a `Memory` (the raw store) **and an `LLMClient` handle supplied explicitly by the user**. When the view would drop exchanges, it compresses those leaving exchanges into one summary message; the request becomes `[system] + [summary] + [recent raw exchanges]`.

- The decision "memory may call an LLM" is confined to this one class; the user opted in by passing the client. `context()` on every other implementation remains pure and non-blocking.
- Summaries are regenerated only when the cut point moves (§7 batching), so the `[system] + [summary]` prefix stays stable and cacheable between cuts.
- Summarization failures degrade gracefully: fall back to plain trimming for that turn; never fail the run because compression failed.
- Deliberately **after** Phase 2: correct summary triggering wants token counting, and cheap bounded memory must exist first (YAGNI).

## 13. Long-term facts — `FactStore` (post-1.0, with the Jarvis demo)

Different question ("which stored facts are relevant to this task?"), different lifetime (across sessions), different dependencies (embeddings + vector store). Therefore **not** a `Memory`:

- Own small interface (store / query by relevance), consulted beside memory — most likely exposed to the model as a `recall_facts` / `remember_fact` tool pair, which fits the existing tool contract and keeps the agent loop unchanged.
- Ships with the Jarvis assistant demo, where the need is real. Keeps embeddings/vector deps out of the core library permanently.

## 14. Usage sites (API-first)

```cpp
// Default — unchanged, honest: keeps everything, right for short tasks.
auto a = AgentBuilder().withModel(m)
    .withSystemPrompt("You are a planner for a warehouse robot.")   // agent config, not memory
    .build();

// Phase 1 — bounded growth for long-running loops (ROS2/game/HFT):
auto b = AgentBuilder().withModel(m)
    .withMemory(inMemory().lastN(20))            // count bound, zero-dep, cache-friendly jumps
    .build();

// Phase 1 — persistent, multi-session:
auto c = AgentBuilder().withModel(m)
    .withMemory(sqlite("agent.db", "user-42").lastN(40))
    .build();

// Phase 2 — real fit guarantee:
auto d = AgentBuilder().withModel(m)
    .withMemory(inMemory().maxTokens(4000))      // clamped, reserve-aware, watermarked
    .build();
auto e = AgentBuilder().withModel(m)
    .withMemory(inMemory().autoWindow())         // derived from the model
    .build();

// Phase 3+ — opt-in summary (chat assistant; accepts extra LLM call):
auto f = AgentBuilder().withModel(m)
    .withMemory(summarizing(sqlite("mem.db", "user-42"), m).keepRecent(20))
    .build();
```

`inMemory()` / `sqlite(path, session)` return a small config handle so bounds read fluently; `build()` resolves it to a store plus a view policy (§4.2). Exact spelling is an implementation-plan detail; the shape above is the contract. (`inMemory()` keeps returning a usable `MemoryPtr` for existing code.)

## 15. Testing (offline, deterministic — per repo rule)

- **View policies:** pure functions over message vectors — unit-test boundary cases directly: pinned-message survival, in-flight exchange survival at any `k`, atomic tool-exchange units (never orphan a `toolCallId`), **cut-point stability** (prefix unchanged for `slack` appends, then one jump), monotonic cut under store eviction, head+tail truncation marker, clamp + reserve arithmetic (with a fake `contextWindow()`).
- **Sanitizer:** orphaned results, unanswered `toolCalls`, partial answers — each dropped as a unit and reported.
- **Atomic commits:** MockLLM (or mock transport) throws on the first call → memory unchanged; throws on iteration 3 → exactly two committed exchanges; a deliberately throwing `Tool` subclass → no orphaned `toolCalls` in memory.
- **System prompt:** sent first on every request, never present in `memory->context()`, survives `clear()`.
- **Backstop:** mock returns `ContextOverflow` once → assert truncate → trim → retry and eventual success, store untouched, learned cap pre-trims the next request; persistently → typed `LLMError{ContextOverflow}`, bounded attempts, intact transcript.
- **SqliteMemory:** session isolation, `appendAll` atomicity (fault injected mid-transaction), schema-version gate (newer version refused), migration from v1 fixture, load-time repair of a corrupted fixture, unknown JSON fields preserved on round-trip. Uses a temp file; no network.
- **Observability:** `onContextTrim` fires once per cut (not per request) and per backstop step; `RunResult` counters sum correctly.
- **SummarizingMemory:** MockLLM plays the summarizer — assert trigger at cut, `[system]+[summary]+[recent]` shape, summary not regenerated between cuts, graceful fallback when the summarizer call fails.
- **Estimator:** property test — estimate within tolerance band on representative corpora; never a hard fit assertion (it's a heuristic).

## 16. Explicit non-goals

- No embeddings, vector store, or retrieval in the core library — ever (FactStore is a separate opt-in component beside it).
- No hidden LLM calls: nothing on the default path may block on a model.
- No universal default N or budget: defaults derive from the model (`autoWindow`) or stay unbounded; we do not hardcode a magic 20.
- No summarization before cheap bounded memory exists (no Phase-1 summary).
- No message content in telemetry.

## 17. Phase checklist and branch mapping

- **Phase 1:**
  - `feat/anthropic-client` (branch 2): `withSystemPrompt`; atomic exchange commits + `Memory::appendAll` (this is when `complete()` starts throwing); client merges consecutive user-role turns; prompt-cache breakpoints (cloud spec).
  - `feat/sqlite-memory` (branch 6): `SqliteMemory` per §10.
  - `feat/memory-trim-backstop` (branch 7): view-policy seam + `lastN` with batching (§7–§8.1); sanitizer (§5); request-level backstop with learned cap (§9); `onContextTrim` + `RunResult` counters (§11); `InMemoryMemory` eviction under a policy (§4.2).
- **Phase 2:** `TokenCounter` seam (estimator default, llama.cpp exact); watermarked `maxTokens` with clamp + output reserve + head/tail truncation; `autoWindow`; `LLMClient::contextWindow()/tokenizer()` capability hooks.
- **Phase 3+:** `SummarizingMemory` (opt-in decorator).
- **Post-1.0 (Jarvis demo):** `FactStore` + `recall_facts`/`remember_fact` tools.

### Revision log

- **v1 (2026-07-04):** composable policies, honest guarantees, trim rules, overflow backstop, phasing.
- **v2 (2026-10-08):** production-hardening review. Added: system prompt as agent config (§4.1); store vs view separation and bounded RAM under a policy (§4.2); request assembly (§4.3); sanitizer (§5); atomic exchange commits + `appendAll` (§6); structural in-flight pin replacing the 4–6 message floor (§7); batched, cache-friendly trimming (§7, §8); request-level backstop with a learned cap that rethrows the typed error instead of a `RunResult` error (§9, aligned with the cloud error contract); SqliteMemory sessions, schema versioning, JSON rows, load-time repair (§10); observability (§11); head+tail truncation. The `Memory` seam is now "additive only" instead of "unchanged" (one new method with a default).
