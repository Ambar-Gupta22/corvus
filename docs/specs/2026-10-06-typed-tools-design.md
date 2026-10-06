# Typed tools — design spec

**Date:** 2026-10-06
**Scope:** `feat/typed-tools` — `corvus::Args` (parsed tool-args view) + `typedTool<T>` (struct-typed tool builder).
**Out of scope:** generic per-call validation for *all* tools incl. MCP, unknown-key rejection, 64 KB arg cap (→ `feat/arg-validation`, which builds on `Args`); nested objects, arrays, enums.

## Goal

In-process C++ tools are corvus's moat over MCP and Python frameworks: they touch the host app's
state (ROS2 topics, game world, order book) with zero IPC. So authoring one must be the nicest part
of the API. Today it is not: the body receives a raw `const std::string&` of JSON and must parse it
by hand (bringing its own JSON library), while a separately written `schema()` chain describes the
same fields to the model. Two sources of truth drift.

Target usage — declare the struct once; schema and parsing are both derived from one field list:

```cpp
struct MoveArgs { double x = 0; double y = 0; std::string frame = "map"; };

auto move = corvus::typedTool<MoveArgs>("move_to", "Move the robot to (x, y)")
    .field("x", &MoveArgs::x, "target x in metres")
    .field("y", &MoveArgs::y, "target y in metres")
    .field("frame", &MoveArgs::frame, "TF frame", corvus::Optional)
    .run([&](const MoveArgs& a) { return robot.moveTo(a.x, a.y, a.frame); });
```

## Decisions

1. **Explicit field registration via pointer-to-member**, not macros or reflection. C++17 has no
   reflection; a macro (`CORVUS_FIELDS(x, y)`) cannot carry per-field descriptions, and descriptions
   are what make the model call the tool correctly. `field(key, &T::m, desc)` deduces the JSON type
   from the member type at compile time.
2. **No third-party type in the public header.** nlohmann/json stays PRIVATE. The header-only
   template talks only to `corvus::Args`, a non-template pimpl class whose implementation (and the
   json include) lives in `src/args.cpp`.
3. **Bad args are `RetryableError`**, rendered by the loop as `ERROR: invalid arguments: <why>`.
   The model produced the args, so the model can fix them on the next turn.
4. **Lenient on extras, strict on declared fields.** Unknown keys are ignored; a declared field of
   the wrong JSON type is an error (no silent string→number coercion). JSON `null` counts as absent,
   since models often send `null` for optional fields. An integer field accepts an exactly integral
   float (`3.0`) because some providers serialise every number as a float.
5. **Builder misuse fails fast** (`std::invalid_argument` at construction): empty or duplicate
   field key. Unsupported member types are a `static_assert`, not a runtime error.

## Components

| Unit | Location | Role |
|------|----------|------|
| `Args` | `include/corvus/args.h`, `src/args.cpp` | Parsed JSON-object view: `parse`, `has`, `getString`/`getNumber`/`getInteger`/`getBool` (each `std::optional`, strict). Usable directly by `Tool` subclass authors. |
| `Requiredness` (`Required`/`Optional`) | `include/corvus/typed_tool.h` | Readable flag instead of a bare `bool`. |
| `TypedToolBuilder<T>` / `typedTool<T>()` | `include/corvus/typed_tool.h` | Records fields into an internal `Schema` + per-field binders; `run(fn)` returns a `ToolPtr` built on the existing full-form `makeTool`. |

### Type mapping

| Member type | JSON Schema type | Notes |
|---|---|---|
| `std::string` | `string` | |
| `bool` | `boolean` | |
| integral (not `bool`/character types) | `integer` | range-checked into the member type |
| floating point | `number` | accepts JSON ints too |
| `std::optional<U>` | type of `U` | always optional; absent → `nullopt` |

An absent optional field keeps the struct's default member initializer.

### Execution path

`execute(text, ctx)` → `Args::parse(text)` (empty text = `{}`; non-object or invalid JSON →
retryable) → run binders in declaration order (first failure → retryable, naming the field) →
call the user function: simple form `std::string(const T&)` → `Ok`; full form
`ToolResult(const T&, const ToolContext&)` → as returned. Exceptions are already converted to
`FatalError` by `FunctionTool::execute`.

## Relation to `feat/arg-validation`

That branch adds loop-level validation for every tool (including MCP tools whose schemas arrive as
text). It should reuse `Args` for parsing rather than add a second JSON path. Typed tools validate
their own fields regardless, so the two layers compose: the generic layer enforces size caps and
schema shape, while the typed layer guarantees the struct is fully populated.
