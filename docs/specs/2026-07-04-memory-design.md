# Memory management design — corvus

**Date:** 2026-07-04
**Status:** Draft for review
**Resolves:** open decision #5 in CLAUDE.md (memory trimming) and expands it into a full memory roadmap.
**Related:** [2026-06-29-jarvis-cpp-design.md](2026-06-29-jarvis-cpp-design.md) (main design), [phase-0-explained.md](../phase-0-explained.md) §5.

## 1. Problem

The LLM is stateless; `Memory` re-sends history every turn. Today `InMemoryMemory` keeps everything forever (unbounded), so any long-running agent eventually overflows the model's context window and the run dies. We need bounded memory — but our two audiences pull in opposite directions:

- **Long-running autonomous loops** (ROS2, game NPC, HFT, embedded): history grows for hours; every turn must stay cheap, deterministic, and free of surprise LLM calls or heavy dependencies.
- **Conversational assistants** (the post-1.0 Jarvis demo): users expect "remember what we said" and eventually "remember facts across sessions"; an extra LLM call for summarization is acceptable.

One fixed design cannot serve both. The design must let each user pay only for what they need.

## 2. Decision summary

**Memory is not a fixed tier hierarchy. It is a set of independent, composable policies behind the existing `Memory` interface.** The original 3-tier sketch (short-term / summary / long-term) survives as three *capabilities*, but they compose differently:

| Capability | Mechanism | Cost profile | Phase |
|---|---|---|---|
| Unbounded history | `InMemoryMemory` (today) | zero; overflows eventually | 0 ✅ |
| Persistence | `SqliteMemory` (same contract, on disk) | zero extra per turn | 1 |
| Short-term bound — count | trimming policy `lastN(k)` | zero-dep, deterministic | 1 |
| Overflow backstop | reactive, in the loop/client | only fires on provider error | 1 |
| Short-term bound — tokens | trimming policy `maxTokens(t)` + `TokenCounter` | needs tokenizer/estimator | 2 |
| Derived budget | `autoWindow()` (budget from model metadata) | exact local, best-effort cloud | 2 |
| Rolling summary | `SummarizingMemory` decorator (holds an `LLMClient`) | +1 LLM call when it fires | 3+ (opt-in) |
| Long-term facts | `FactStore` — separate interface, NOT a `Memory` | embeddings/vector store, opt-in | post-1.0 (with Jarvis demo) |

Core rules:

1. **The `Memory` interface does not change.** `append` / `context` / `clear` stays the seam ([memory.h](../../include/corvus/memory.h)). Everything below is implementations and decorators behind it.
2. **The base path stays pure.** Plain stores (`InMemoryMemory`, `SqliteMemory`) and trimming policies never do I/O to a model and never require a JSON/HTTP/embedding dependency. Only the explicitly opt-in `SummarizingMemory` may call an LLM.
3. **Nothing is removed as options are added.** Unbounded stays the default; bounds are opt-in. The menu grows: P0 unbounded → P1 +lastN → P2 +maxTokens/autoWindow → P3+ +summary.
4. **Long-term memory is not on the `context()` path.** Retrieval is query-driven and dependency-heavy; it gets its own interface (`FactStore`), surfaced beside memory (likely as a `recall_facts` tool), never baked into the decorator chain.

## 3. What each bound actually guarantees (be honest in docs)

- **unbounded** — guarantees nothing; will overflow eventually. Right for short tasks.
- **`lastN(k)`** — bounds *growth* (no infinite accumulation). Does **not** guarantee fit: k fat messages (large tool outputs) can still exceed the window. It is a cheap safety valve, not a correctness guarantee. Document this loudly.
- **`maxTokens(t)`** — bounds *size*; guarantees fit **to the budget t**, subject to the three edge cases in §5. This is the real fix; `lastN` is the dependency-free stopgap that ships first.

Public docs must state: long-running agents **must** set a bound; short chats need not.

## 4. Trimming rules (apply to every policy)

Trimming only ever drops **old, completed** turns, oldest first. It must never drop:

1. **The system message.** Always kept, regardless of policy or pressure.
2. **The current in-flight exchange** — the active user task and any assistant/tool turns of the iteration in progress. Practical floor ≈ 4–6 messages; a policy configured below the floor is a builder validation error.
3. **Half of a tool exchange.** The hardened `Message` pairs an assistant turn's `toolCalls` with subsequent `role=="tool"` messages by `toolCallId` ([types.h](../../include/corvus/types.h)). Providers reject orphaned tool results. **Trim at turn boundaries:** an assistant-with-toolCalls message and all its paired tool results are one atomic unit — drop all or none.

## 5. Token budgeting details (Phase 2)

### 5.1 TokenCounter is pluggable, per-backend

Tokenization is per model family; there is no universal tokenizer, and we do not implement one:

- **`CountEstimator`** (default, universal, dep-free): heuristic ≈ chars/4. Used for trim decisions when nothing better exists.
- **`ExactCounter`** (per backend, when available): llama.cpp exposes `llama_tokenize()` for the loaded GGUF — exact, offline, free (this is why token budgeting lands in Phase 2 with local backends). OpenAI has tiktoken ports (extra dep, opt-in). Anthropic has no offline tokenizer — estimator only; the API's returned `usage` gives exact numbers *post-hoc* and feeds `RunResult` cost reporting, not the trim decision.

`LLMClient` grows two optional capability hooks (default: "unknown"): `contextWindow()` and `tokenizer()`. Backends that know, report; backends that don't leave the estimator in charge.

### 5.2 Budget derivation — don't trust the raw number

Three edge cases break a naive `maxTokens(t)`:

1. **Single oversized message > whole budget** (giant tool output). Trimming count can never fix this; the policy truncates the individual message with a `[...truncated]` marker. Prevention lives at the source too: the `ToolGuard` **output cap** (main design doc) truncates huge tool results before they enter memory. Same problem attacked from both ends.
2. **User budget > model window.** Clamp: `effective = min(t, window − reserve)`; builder warns loudly at build time when it can see the mismatch ("maxTokens 8000 exceeds model window 4096").
3. **No output headroom.** The window is input + output combined; packing it full leaves the model no room to answer. Reserve `maxOutputTokens + safetyMargin` before filling.

`autoWindow()` is the smart default that sidesteps all three: derive the budget from the model (`window − maxOutput − margin`) instead of trusting a hand-entered number. Exact for local backends (GGUF metadata); config-table/best-effort for cloud.

## 6. Overflow backstop (Phase 1, always on)

Proactive policies reduce overflow; they cannot eliminate it (model swaps, estimator error, unbounded chosen). The loop/client therefore handles the provider's `context_length_exceeded` reactively, as a fixed sequence — not a user choice:

1. Truncate any single oversized message (marker as in §5.2).
2. Trim old turns harder (respecting §4 rules).
3. Retry the call (bounded retries, shares Phase-1 retry/backoff machinery).
4. Still failing → return a clean, actionable error in `RunResult`; never crash, never silently drop the run.

Belt (policy, proactive) + suspenders (backstop, reactive). The backstop exists under **all** policies including unbounded.

## 7. Rolling summary — `SummarizingMemory` (Phase 3+, opt-in)

Decorator: is-a `Memory`, has-a `Memory` (the raw store) **and an `LLMClient` handle supplied explicitly by the user**. When the raw history exceeds its threshold, it compresses old completed turns into one summary message; `context()` returns `[system] + [summary] + [recent raw turns]`.

- The decision "memory may call an LLM" is confined to this one class; the user opted in by passing the client. `context()` on every other implementation remains pure and non-blocking.
- Summarization failures degrade gracefully: fall back to plain trimming for that turn; never fail the run because compression failed.
- Deliberately **after** Phase 2: correct summary triggering wants token counting, and cheap bounded memory must exist first (YAGNI).

## 8. Long-term facts — `FactStore` (post-1.0, with the Jarvis demo)

Different question ("which stored facts are relevant to this task?"), different lifetime (across sessions), different dependencies (embeddings + vector store). Therefore **not** a `Memory`:

- Own small interface (store / query by relevance), consulted beside memory — most likely exposed to the model as a `recall_facts` / `remember_fact` tool pair, which fits the existing tool contract and keeps the agent loop unchanged.
- Ships with the Jarvis assistant demo, where the need is real. Keeps embeddings/vector deps out of the core library permanently.

## 9. Usage sites (API-first)

```cpp
// Default — unchanged, honest: keeps everything, right for short tasks.
auto a = AgentBuilder().withModel(m).build();

// Phase 1 — bounded growth for long-running loops (ROS2/game/HFT):
auto b = AgentBuilder().withModel(m)
    .withMemory(inMemory().lastN(20))            // count bound, zero-dep
    .build();

// Phase 2 — real fit guarantee:
auto c = AgentBuilder().withModel(m)
    .withMemory(inMemory().maxTokens(4000))      // clamped, reserve-aware
    .build();
auto d = AgentBuilder().withModel(m)
    .withMemory(inMemory().autoWindow())         // derived from the model
    .build();

// Phase 3+ — opt-in summary (chat assistant; accepts extra LLM call):
auto e = AgentBuilder().withModel(m)
    .withMemory(summarizing(sqlite("mem.db"), m).keepRecent(20))
    .build();
```

`inMemory()` / `sqlite(path)` return a small config handle so bounds read fluently; `build()` resolves it to the right implementation. Exact spelling is an implementation-plan detail; the shape above is the contract.

## 10. Testing (offline, deterministic — per repo rule)

- **Trim policies:** pure functions over message vectors — unit-test boundary cases directly: system message survival, floor enforcement, atomic tool-exchange units (never orphan a `toolCallId`), oversized-message truncation, clamp + reserve arithmetic (with a fake `contextWindow()`).
- **Backstop:** MockLLM (or mock transport) returns `context_length_exceeded` once → assert truncate/trim/retry sequence and eventual success; returns it persistently → assert clean `RunResult` error, no crash, bounded retries.
- **SummarizingMemory:** MockLLM plays the summarizer — assert trigger threshold, `[system]+[summary]+[recent]` shape, and graceful fallback when the summarizer call fails.
- **Estimator:** property test — estimate within tolerance band on representative corpora; never a hard fit assertion (it's a heuristic).

## 11. Explicit non-goals

- No embeddings, vector store, or retrieval in the core library — ever (FactStore is a separate opt-in component beside it).
- No hidden LLM calls: nothing on the default path may block on a model.
- No universal default N or budget: defaults derive from the model (`autoWindow`) or stay unbounded; we do not hardcode a magic 20.
- No summarization before cheap bounded memory exists (no Phase-1 summary).

## 12. Phase checklist (delta to roadmap docs)

- **Phase 1:** `SqliteMemory`; `lastN` trimming policy (+ §4 rules); overflow backstop in loop/client; builder floor validation. *(CLAUDE.md decision #5 currently says only "keep last N + keep system prompt" — superseded by this doc: same ship item, plus backstop + turn-boundary rules.)*
- **Phase 2:** `TokenCounter` seam (estimator default, llama.cpp exact); `maxTokens` with clamp + output reserve + oversized-message truncation; `autoWindow`; `LLMClient::contextWindow()/tokenizer()` capability hooks.
- **Phase 3+:** `SummarizingMemory` (opt-in decorator).
- **Post-1.0 (Jarvis demo):** `FactStore` + `recall_facts`/`remember_fact` tools.
