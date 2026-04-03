## Context

`clice` already has the rough shape of a language server: a master process, stateful/stateless workers, compile scheduling, module/PCH support, an emerging index, and a set of AST-backed editor features. The current gap is not "missing framework", but a mismatch between the current implementation shape and the invariants required for real editor use.

The highest-risk issues are all cross-cutting:

- opened editor buffers are not yet the consistently authoritative source across compile, AST queries, and index-backed navigation
- stateful feature reads can race with rebuilds and stale generations
- header files and multi-context files do not yet have a sound compilation-context selection model
- several features are either missing, stubbed, or substantially shallower than `clangd`
- worker ownership, eviction, diagnostics, logging, and background work scheduling are not yet production-grade

This design therefore treats production readiness as four coordinated workstreams with explicit boundaries: document model, compilation context, AST-only feature baseline, and runtime hardening.

## Goals / Non-Goals

**Goals:**

- make opened-buffer semantics explicit and enforceable across compile and query paths
- define how `clice` selects and persists compilation contexts for sources and headers
- set a concrete AST-only feature baseline that can be implemented independently from global index work
- define runtime invariants for worker lifecycle, resource control, logging, and stale-result handling
- produce a task breakdown that allows server/core work and feature work to proceed in parallel with minimal overlap

**Non-Goals:**

- full global-index parity with `clangd`
- final implementation details for every indexed feature such as workspace-wide rename or workspace symbols
- performance tuning beyond the invariants needed to avoid obviously wrong scheduling and stale results
- introducing compatibility layers or temporary abstractions whose only purpose is to avoid refactoring existing wrong-shape code

## Decisions

### 1. Treat the opened document as the primary source of truth

Open documents with unsaved changes must override disk content for compile and AST-backed queries. This applies to on-demand compiles, stateful feature reads, and the local side of navigation requests.

Why this over disk-first fallback:

- it matches editor expectations
- it removes offset mismatches between the text used to compute positions and the AST used to answer them
- it gives a single invariant that can be tested across hover, completion, semantic tokens, and local navigation

Alternative considered:

- continue using disk-backed index and best-effort stale tolerance
  Rejected because it produces silently wrong answers in the most common editing flow.

### 2. Gate AST-backed feature reads on a versioned build generation

Every opened document needs a version/generation model that ties together text, build scheduling, worker state, AST readiness, diagnostics publication, and feature requests. AST-backed reads must either use a generation known to match the current document or fail/retry cleanly; they must not run on a stale or fatal unit by default.

Why this over the current dirty-flag shape:

- `dirty` alone records staleness but does not protect the read path
- version/generation checks let master and worker reject stale compile completions and stale feature answers deterministically

Alternative considered:

- optimistic reads while a rebuild is in flight
  Rejected because it preserves responsiveness at the cost of correctness and makes debugging behavior impossible.

### 3. Separate compilation-context resolution from feature implementation

Header context, compilation context, and multi-command source handling must be solved in the compile layer, not feature-by-feature. Feature handlers should receive a coherent AST/index view and not carry their own header fallback logic.

Why this over ad hoc per-feature fallback:

- it prevents each feature from inventing a different answer for the same file
- it keeps AST-only feature work independently assignable
- it matches the way `clangd` centralizes command transfer and TU scheduling

Alternative considered:

- keeping generic fallback compile commands for headers until later
  Rejected because it creates misleading "working" behavior with the wrong language mode and include environment.

### 4. Keep AST-only feature work independent from index work

The feature baseline for this change covers capabilities that can work from the current AST or local parse context. Missing index-backed features may be tracked, but they are not on the critical path for this change.

Why this split:

- it allows server/core work to proceed without blocking feature contributors
- it matches the current repository structure, where most AST features already live under `src/feature`
- it gives a realistic route to internal dogfooding before full index maturity

Alternative considered:

- waiting for index parity before improving features
  Rejected because it serializes unrelated work and delays useful editor quality improvements.

### 5. Remove or finish wrong-shape/stub behavior instead of hiding it

Capabilities that are currently advertised but stubbed, or data structures that exist without a correct runtime selection model, should either be finished or explicitly de-scoped. Production readiness must reduce misleading behavior, not preserve it.

Why this over compatibility shims:

- stubs make clients believe a feature exists when it does not
- partial lifecycle plumbing causes leaks and stale ownership even if the server appears responsive
- cleaning up wrong-shape code reduces the long-term cost of later index and context work

Alternative considered:

- preserve current surface and patch around it incrementally
  Rejected because it accumulates technical debt exactly in the areas that most need stable invariants.

### 6. Use one research document as the coordination artifact, then execute by small work packages

This change produces a detailed markdown report under `temp/` that groups findings into server/core and feature workstreams, then translates them into small, independently assignable tasks. The report is not the contract itself; the specs are. The report exists to preserve comparison context and justify prioritization.

Why this over burying all detail in tasks:

- the comparison against local `clangd` sources is too detailed to keep only in task bullets
- the report gives future implementation agents a single place to understand gaps and evidence

## Risks / Trade-offs

- [Stricter generation gating may temporarily increase null/error responses] -> Mitigation: prefer explicit stale-result rejection over silently wrong data, and add targeted tests for retry behavior.
- [Header-context design can expand into full index work] -> Mitigation: keep this change focused on command/context selection and defer cross-project index heuristics that are not needed for production dogfooding.
- [Feature parity work may accidentally duplicate already-completed behavior] -> Mitigation: each feature task must confirm current implementation state before coding, using the gap-analysis report and local source references.
- [Cleaning up wrong-shape code may require deleting partially integrated paths] -> Mitigation: treat deletion as part of completion criteria rather than as optional follow-up.
- [Parallel execution can produce merge conflicts around `master_server.cpp` and protocol definitions] -> Mitigation: split tasks by ownership boundary, with server/core work owning protocol and lifecycle changes, and feature work owning only their feature modules plus the minimal routing needed for exposure.

## Migration Plan

1. Land the planning artifacts and the gap-analysis document.
2. Implement server/core invariants first where feature correctness depends on them:
   open-buffer source of truth, generation-safe AST reads, header/context selection, lifecycle cleanup.
3. In parallel, implement AST-only feature tasks that do not depend on index maturity.
4. Remove or hide stubbed feature advertisements that remain incomplete.
5. Expand test coverage around document lifecycle, stale-result rejection, header handling, and feature-level golden cases.
6. Start internal dogfooding only after the server/core acceptance criteria and the minimum AST-only baseline are both satisfied.

Rollback strategy:

- because this change is primarily a planning and refactoring contract, rollback means disabling newly advertised features or reverting individual implementation tasks rather than reverting the entire capability set at once

## Open Questions

- How should `clice` choose among multiple valid compilation contexts for the same source file before the full index is mature?
- For headers without a direct compile command, what ranking rule should choose the preferred includer or proxy translation unit?
- Should AST-backed requests block until a matching generation is ready, or return an explicit retriable error after a short timeout?
- Which AST-only code actions are mandatory for first internal rollout, and which can remain behind capability flags?
- How should dynamic, unsaved-document symbol data be combined with disk-backed merged index results without reintroducing offset drift?
