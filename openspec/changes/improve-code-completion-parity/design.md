## Context

`clice` currently routes `textDocument/completion` through `MasterServer::forward_stateless()`, which prepares open-buffer text plus available PCH/PCM paths and sends a one-shot completion request to the stateless worker. The worker builds `CompilationParams`, invokes `feature::code_complete()`, and returns a raw `std::vector<CompletionItem>`.

The implementation in `src/feature/code_completion.cpp` is intentionally small:

- run Sema code completion at a single file/offset
- compute the local identifier prefix
- fuzzy-match candidate labels against the typed prefix
- emit only basic LSP fields such as `label`, `kind`, `textEdit`, and `sortText`
- bundle function overloads only by qualified name, with `detail = "(...)"` for grouped overloads

Compared with the local `clangd` implementation in `.llvm/clang-tools-extra/clangd`, the gap is large and structural rather than cosmetic. `clangd` has a full completion pipeline:

- request shaping from client capabilities and trigger context in `ClangdLSPServer`
- `CodeCompleteFlow` as a staged completion engine rather than direct Sema rendering
- candidate fusion from Sema, project index, and raw identifiers
- context capture such as completion kind, query scopes, expected type, and following token
- scoring that combines name match with semantic/contextual signals
- richer LSP rendering including signatures, return types, docs, snippets, fix-its, additional edits, deprecation, `filterText`, `isIncomplete`, and include insertion

At the same time, `clice` has infrastructure that clangd either lacks or does not currently integrate into completion as deeply:

- explicit header-context modeling for headers shared by multiple includers
- existing PCH/PCM request plumbing in the master/stateless worker path
- merged per-file index shards and project-wide symbol/reference data
- a project goal of instantiation-aware template processing

There is also a hard constraint: `clice`'s current index symbol payload only stores `name`, `kind`, and reference files. That is enough for navigation-oriented lookup, but not enough for clangd-style index completion, which needs scope, signature, return type, documentation, deprecation, canonical declaration/header ownership, and dependency insertion hints.

## Goals / Non-Goals

**Goals:**

- close the highest-value completion gap with clangd in behavior, not in file-by-file implementation shape
- replace the current direct Sema-to-LSP path with a staged completion pipeline
- support rich, capability-aware LSP completion responses
- augment Sema completion with identifier and project-index candidates
- introduce heuristics that use semantic/contextual signals instead of raw fuzzy score alone
- define how fix-its and dependency insertion edits attach to completion items
- use `clice`'s header-context, module, and template machinery to define completion behaviors clangd does not currently provide

**Non-Goals:**

- byte-for-byte parity with clangd internals or its exact scoring constants
- introducing `completionItem/resolve` in this change
- shipping a learned ranking model in the first implementation; heuristic ranking is sufficient
- requiring the global index to become fully clangd-equivalent before completion quality improves
- solving every completion-related editor quirk across all clients before the core pipeline exists

## Decisions

### 1. Split completion into request shaping, candidate collection, scoring, and rendering

`clice` should stop treating code completion as "run Sema and immediately serialize `CompletionItem`s". Instead, completion will become a staged pipeline:

1. request shaping at the LSP/server edge
2. candidate collection in the worker
3. merge/deduplicate/bundle
4. score and truncate
5. render to LSP according to client capabilities

Why this approach:

- it matches the actual gap with clangd, which is pipeline depth rather than one missing field
- it keeps protocol negotiation in the server and semantic work in the worker
- it allows `clice`-specific signals to affect ranking without polluting the LSP layer

Alternative considered:

- continue returning `std::vector<CompletionItem>` directly from `feature::code_complete()`
  Rejected because `isIncomplete`, score-aware truncation, richer origins, and delayed rendering all need an intermediate completion model.

### 2. Extend the completion request contract with client capabilities and trigger context

The current stateless completion request only carries file text, compile arguments, and offset. The request contract should be extended so the worker can render results correctly:

- completion trigger kind/character
- client snippet support
- documentation format preference
- label-details support
- supported completion item kinds
- requested limit
- overload-bundling preference

The master server should negotiate these from `initialize` capabilities and pass a normalized completion-options struct to the worker.

Why this approach:

- `clangd` already proves that completion quality depends on client capabilities
- several existing `CodeCompletionOptions` fields in `clice` are never meaningfully driven by the LSP client today
- it keeps editor-specific policy out of the core collector logic

Alternative considered:

- hardcode one rendering mode for all clients
  Rejected because it prevents snippet/doc behavior from ever becoming correct and makes the existing options struct mostly dead code.

### 3. Keep Sema as the primary source of truth, but augment it with identifiers and index symbols

Collection order should be:

- Sema candidates from the active compilation context
- local/raw identifiers from the open buffer and active file context
- project/index candidates when the completion context allows them

Sema remains primary because it understands visibility, module state, recovery fix-its, and the exact active compile command. Identifier and index sources fill gaps when Sema is incomplete or when project-wide results are useful.

Merge rules should deduplicate by insertion semantics, not just by display label. Function overload bundling should group only candidates that render identically and require the same dependency edits.

Why this approach:

- it preserves correctness for the current file while still expanding recall
- it fits `clice`'s current stateless worker path, which already builds the correct compile inputs
- it allows richer completion without waiting for index-only correctness

Alternative considered:

- move to index-first completion with Sema as fallback
  Rejected because `clice`'s strongest current signals for modules, fix-its, and header contexts come from active compilation, not from the persisted index.

### 4. Expand the index schema specifically for completion metadata

`clice`'s current `index::Symbol` payload is too small for rich completion. To support index-backed completion, the index data model should be extended with completion-oriented fields such as:

- qualified scope / shortest usable qualifier
- signature and return type
- deprecation flag
- documentation summary or documentation lookup key
- canonical declaration path
- preferred include header or module/import source
- symbol origin/category for ranking

The first implementation does not need every clangd field, but it does need enough metadata to render and rank project-index candidates meaningfully.

Why this approach:

- without extra index payload, project-index completion can only return weak name-only suggestions
- dependency insertion cannot be justified without declaration ownership metadata
- the serialization boundary already exists in `TUIndex`, `ProjectIndex`, and merged shards, so this is the right architectural layer

Alternative considered:

- use the current index unchanged and limit project-index completion to `name + kind`
  Rejected because it would add complexity without delivering clangd-level value, and would likely need to be rewritten immediately after.

### 5. Use heuristic ranking first, and encode the signals explicitly

The first ranking model should remain heuristic, but it must go beyond raw fuzzy score. Candidate scoring should combine:

- prefix/exact name match
- semantic availability from Sema
- in-scope vs required-qualifier insertion
- expected type / preferred type compatibility when available
- file/header-context proximity
- deprecation penalty
- module availability and dependency-edit cost
- template-instantiation compatibility when `clice` can infer it

This keeps the design testable and explainable without blocking on a learned model.

Why this approach:

- it delivers most user-visible value quickly
- it maps naturally onto `clice`'s existing semantics-heavy architecture
- it leaves room for a later learned ranker without locking the system into clangd's exact model

Alternative considered:

- adopt clangd-style decision-forest ranking immediately
  Rejected because `clice` does not yet have the surrounding instrumentation and labeled data pipeline, and heuristic ranking is sufficient for this change.

### 6. Treat completion-related edits as first-class output, with staged support

Two classes of edits matter:

- Sema-provided fix-its near the completion site
- dependency insertion edits such as `#include` or `import`

The completion model should carry both as structured edits and render them into LSP `additionalTextEdits` or merged `textEdit`s where appropriate.

Implementation should be staged:

- stage 1: preserve and emit Sema fix-its
- stage 2: add dependency insertion for header-backed symbols once index metadata can justify it
- stage 3: add module import insertion where the active TU and candidate source indicate a module-based dependency is the right form

Why this approach:

- it closes a large clangd parity gap without forcing all edit types at once
- module-aware insertion is a place where `clice` can exceed clangd's current include-centric design

Alternative considered:

- postpone all completion edits until after ranking/index parity
  Rejected because Sema fix-its are already available and should not be discarded.

### 7. Make `clice`-specific header, module, and template context part of completion semantics

The main "beyond clangd" opportunities should not be separate side features. They should influence the same completion pipeline:

- **Header context**: completion in a header should use the active includer/source context for that header, instead of treating the header as a single global state.
- **Modules**: available PCM/module dependencies should affect both candidate visibility and dependency insertion strategy.
- **Template instantiation**: when `clice` can identify concrete instantiation or expected-type information, ranking should prefer candidates compatible with that instantiated context.

This means the primary completion request must always be evaluated in one active compilation context, not a union of all possible contexts. Alternate contexts can be explored later, but blindly merging them would produce unstable and misleading completions.

Why this approach:

- it uses the architecture `clice` already claims as a differentiator
- it avoids the worst failure mode for shared headers: mixing incompatible contexts into one result set
- it keeps "beyond clangd" work aligned with user-visible completion quality

Alternative considered:

- add cross-context completion by merging all known header contexts
  Rejected because it sacrifices correctness and predictable ranking for recall.

## Risks / Trade-offs

- [Index payload expansion increases serialization and background-index cost] → Mitigation: add only completion-relevant fields first and measure shard-size growth before expanding documentation payloads further.
- [Ranking heuristics become opaque or brittle] → Mitigation: keep signals explicit, loggable, and unit-tested rather than encoding hidden constants throughout the collector.
- [Header-context-aware completion can produce confusing behavior if the active context is wrong] → Mitigation: bind completion to the same active header-context selection model used by navigation/AST features, not an ad hoc completion-only heuristic.
- [Dependency insertion can apply the wrong form (`#include` vs `import`)] → Mitigation: gate edits on explicit symbol metadata and active compilation mode, and prefer no edit over a wrong edit.
- [More completion metadata increases protocol churn between master and stateless workers] → Mitigation: add a dedicated options/result struct instead of spreading new fields loosely across existing request params.

## Migration Plan

1. Extend the LSP/server edge to capture completion capabilities, trigger context, and limits.
2. Introduce an internal completion result model and return a completion list shape with `isIncomplete`.
3. Refactor the worker pipeline to separate candidate collection, merging, scoring, and rendering while preserving current Sema behavior.
4. Add local-identifier completion and heuristic ranking improvements.
5. Expand the index schema and integrate project-index candidates.
6. Add completion-related edits, first for Sema fix-its and then for dependency insertion.
7. Integrate header-context, module, and template-instantiation signals into ranking and edit policy.
8. Expand unit and integration coverage around rendering, ranking, index augmentation, module-aware completion, and shared-header contexts.

Rollback strategy:

- keep Sema-only completion as a safe fallback path until the staged pipeline is stable
- disable index augmentation or dependency insertion independently if they prove noisy
- prefer full completion results without advanced edits over partially wrong completion behavior

## Open Questions

- How much documentation should be stored directly in `clice`'s index versus fetched/lazily summarized from declarations on demand?
- Should project-index completion support all-scopes insertion from the first version, or require explicit qualifiers only after scope metadata is fully modeled?
- What is the right UX for exposing the currently active header context to users when completion results differ across includers?
- How aggressive should template-instantiation-aware ranking be before users perceive it as "hiding" generic but still legal completions?
- Whether dependency insertion for modules should be represented as ordinary additional text edits or as a future code-action-style completion extension.
