## Context

This change extracts decision `2` from `openspec/changes/explore-improve-folding-range-support/design.md` into a standalone proposal. The current folding implementation in `src/feature/folding_ranges.cpp` mixes three responsibilities in one path:

- discovering foldable structure from AST data
- deciding which ranges survive deduplication and validation
- shaping the final LSP response, including output metadata

That coupling makes the code harder to extend safely. Comment folding, directive-based collectors, capability-aware rendering, and range limiting all become riskier when collection and rendering rules share the same code path. The extracted proposal keeps scope narrower: it does not add new fold categories by itself, but it creates the architecture that later changes can build on without destabilizing existing structural folding.

## Goals / Non-Goals

**Goals:**

- Separate folding processing into collection, normalization, and rendering phases.
- Preserve the existing AST structural folding categories already supported by `clice`.
- Make ordering, deduplication, and boundary validation deterministic and testable.
- Give later changes a stable extension point for comments, directives, and client-driven rendering options.

**Non-Goals:**

- Add comment folding in this change.
- Fix preprocessor branch-closing behavior in this change.
- Add new fold categories such as macro definitions or include/import grouping.
- Depend on initialize-time client capability plumbing being implemented first.

## Decisions

### 1. Introduce a raw internal folding-range model

Collectors should emit an internal `RawFoldingRange`-style structure instead of final LSP protocol objects. The raw model should preserve source locations, an internal category, and optional metadata hints that later phases may use.

Why:

- collectors should describe what was found, not how it will be serialized
- future comment and directive collectors can share the same pipeline contract
- tests can validate collection independently from rendering

Alternative considered:

- Continue emitting LSP ranges directly from collectors. Rejected because it keeps protocol concerns entangled with source discovery.

### 2. Normalize ranges before rendering

All collected ranges should pass through a normalization step before any response is emitted. Normalization is responsible for deterministic ordering, duplicate removal, and rejection of degenerate or unmappable ranges.

Why:

- duplicate or invalid ranges are easier to reason about in one place than across many collectors
- stable ordering reduces regression noise and makes range limiting predictable later
- normalization lets new collectors plug in without each collector re-implementing cleanup logic

Alternative considered:

- Let each collector manage its own sorting and duplicate suppression. Rejected because cross-collector interactions would still remain undefined.

### 3. Keep the current AST visitor as the first collector boundary

The initial extraction should preserve the current AST visitor as one collector feeding the raw model. This reduces refactor risk while still creating the new phase boundaries.

Why:

- the existing structural fold coverage is valuable and should not be rewritten unnecessarily
- an adapter-style refactor is easier to verify against current tests than a full collector redesign

Alternative considered:

- Rewrite collection around a brand-new multi-source manager immediately. Rejected because it adds scope before the phase split is proven.

### 4. Move output shaping into a dedicated renderer

The renderer should translate normalized ranges into LSP folding ranges. Boundary shaping, output kinds, and optional metadata emission should live there, even if some options still use default values until later protocol plumbing exists.

Why:

- rendering rules are a separate concern from source discovery
- later work on line-only output, metadata gating, or public kind mapping should not force collector rewrites
- isolating rendering makes behavioral diffs easier to review

Alternative considered:

- Keep final boundary shaping next to the AST collector and only add a small helper for sorting. Rejected because it only moves a symptom, not the architectural problem.

## Risks / Trade-offs

- [Refactoring the current path can accidentally change fold ordering] -> Mitigation: add deterministic-order assertions and compare outputs for existing structural fixtures.
- [The raw model could become too abstract too early] -> Mitigation: keep the initial fields minimal and only include data already needed by current structural folds.
- [A renderer abstraction may appear premature before full capability plumbing exists] -> Mitigation: keep default render options aligned with current behavior and treat future options as extension points, not immediate scope.

## Migration Plan

1. Introduce raw folding-range and render-option types behind the existing entrypoint.
2. Convert the current AST-based collectors to emit raw ranges.
3. Insert normalization between collection and response emission.
4. Move LSP object construction into a dedicated renderer.
5. Verify that existing structural folding fixtures still produce the expected ranges.

Rollback strategy:

- If the refactor destabilizes output, keep the new helper types but temporarily route the old direct-emission path until normalization and rendering regressions are resolved.

## Open Questions

- Whether public kind remapping should land in this extracted change or remain a follow-up proposal once the renderer boundary exists.
