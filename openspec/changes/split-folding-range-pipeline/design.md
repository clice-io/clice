## Context

This change extracts decision `2` from `openspec/changes/explore-improve-folding-range-support/design.md` into a standalone proposal. The current folding implementation in `src/feature/folding_ranges.cpp` already has an internal `RawFoldingRange` handoff, but it still leaves two important concerns too implicit:

- deciding which ranges survive deduplication and validation
- shaping the final LSP response, including client-specific output rules such as `line_folding_only`

Follow-up discussion clarified that the existing `RawFoldingRange` shape is finished for this extracted change. The remaining architectural gap is an explicit options path, passed as `Opts`/`FoldingRangeOptions`, so rendering can be configured without reworking collectors. This proposal therefore keeps scope narrow: it does not add new fold categories or redesign raw ranges, but it creates the normalization and options-driven rendering boundaries that later changes can build on without destabilizing existing structural folding.

The downloaded clangd reference confirms both the value and the limit of the upstream design. clangd has useful, tested folding behavior for brace bodies, comment blocks, contiguous `//` groups, and `lineFoldingOnly`, but its implementation largely emits protocol-shaped `FoldingRange` objects directly from collection code. In `SemanticSelection.cpp`, both the AST path and the pseudo-parser path build `FoldingRange` results directly, and the pseudo-parser applies rendering details such as delimiter trimming and `lineFoldingOnly` adjustments while collecting ranges. That is a good behavior reference, but it is not the architecture this extracted change should copy.

`clice` already has stronger ingredients for a real pipeline:

- the existing `RawFoldingRange` gives collectors a feature-local representation that is not a final LSP response
- `LocalSourceRange` gives us a main-file, half-open offset representation that is independent of LSP position encoding
- directive metadata already captures information clangd does not expose well, including conditional-branch state, pragma regions, includes, imports, and macro references
- the current tests are boundary-oriented, which makes them a good fit for validating raw spans before protocol rendering

The design therefore separates "what fold exists in the source" from "how that fold should be emitted to this client". clangd's tested boundary rules are still relevant, but they should become renderer policy selected by options and normalization rules rather than collector output format.

## Goals / Non-Goals

**Goals:**

- Keep the existing `RawFoldingRange` collection contract stable while completing normalization and rendering boundaries.
- Preserve the existing AST structural folding categories already supported by `clice`.
- Make ordering, deduplication, and boundary validation deterministic and testable.
- Add an explicit folding options object so `line_folding_only` can be configured by callers and consumed only by rendering.
- Give later changes a stable extension point for comments, directives, and client-driven rendering options.

**Non-Goals:**

- Add comment folding in this change.
- Fix preprocessor branch-closing behavior in this change.
- Add new fold categories such as macro definitions or include/import grouping.
- Redesign or replace the existing `RawFoldingRange` model.
- Depend on initialize-time client capability plumbing being implemented first.

## Decisions

### 1. Use clangd as a behavior reference, not an architecture template

This change should borrow clangd's confirmed folding behavior where it is useful, especially around multiline comments, contiguous `//` comment groups, main-file-only filtering, and `lineFoldingOnly` boundary shaping. It should not copy clangd's habit of emitting protocol-shaped `FoldingRange` objects directly from collection logic.

Why:

- clangd's tests are valuable because they pin down tricky folding behavior around comments, macro boundaries, and line-only rendering
- clangd's data flow is intentionally narrow and mixes collection with response shaping
- `clice` already has richer file-local and directive metadata that supports a cleaner internal representation

Alternative considered:

- Treat clangd's direct `FoldingRange` construction as the architecture to reproduce. Rejected because it would preserve the same coupling this extracted change is meant to remove.

### 2. Treat the existing raw internal folding-range model as finished

Collectors should continue to emit the existing internal `RawFoldingRange` structure instead of final LSP protocol objects. The raw model is sufficient for this extracted change and should not be redesigned as part of adding `line_folding_only` support.

The raw model should remain shaped around file-local source structure, not client capability state. In the current implementation it carries:

- a main-file `LocalSourceRange` span using half-open byte offsets
- an optional public folding kind to preserve existing behavior
- an optional collapsed-text hint

```cpp
struct RawFoldingRange {
    LocalSourceRange range;
    std::optional<protocol::FoldingRangeKind> kind;
    std::string collapsed_text;
};
```

The important design choice is that `range` represents the foldable source envelope in the main file while client-specific rendering state stays out of the raw model. For example:

- brace-based structural folds keep their source span and let the renderer decide line-only boundary shaping
- future block comments can keep the full `/* ... */` span and let the renderer decide whether to hide the closing delimiter or final line
- future contiguous `//` groups can keep the grouped span and let the renderer decide line-only output

Why:

- collectors should describe what was found, not how it will be serialized
- `LocalSourceRange` is already the natural coordinate system for `clice`
- future comment and directive collectors can share the same pipeline contract
- tests can validate collection independently from rendering
- the missing `line_folding_only` behavior belongs in options and rendering, not in the raw range shape

Alternatives considered:

- Continue emitting LSP ranges directly from collectors. Rejected because it keeps protocol concerns entangled with source discovery.
- Expand `RawFoldingRange` now with render-hint fields for line-only behavior. Rejected because follow-up discussion established the raw model as finished for this slice, and line-only support can be configured through rendering options instead.

### 3. Normalize ranges before rendering

All collected ranges should pass through a normalization step before any response is emitted. Normalization is responsible for deterministic ordering, duplicate removal, and rejection of degenerate or unmappable ranges.

Normalization should operate on raw spans and raw metadata, not on already-rendered LSP line/character fields. Its responsibilities include:

- deterministic ordering independent of collector traversal order
- duplicate collapse for collectors that discover the same fold
- invalid-range filtering after raw spans are mapped and validated
- stable tie-breaking for overlapping ranges from different origins

Collectors may still reject obviously invalid inputs, such as non-main-file locations that cannot be mapped to `LocalSourceRange`, but normalization remains the phase that decides which collected folds survive to rendering.

Why:

- duplicate or invalid ranges are easier to reason about in one place than across many collectors
- stable ordering reduces regression noise and makes range limiting predictable later
- metadata-aware normalization preserves fold meaning until the renderer maps it to public output
- normalization lets new collectors plug in without each collector re-implementing cleanup logic

Alternative considered:

- Let each collector manage its own sorting and duplicate suppression. Rejected because cross-collector interactions would still remain undefined.

### 4. Keep the current AST visitor as the first collector boundary

The initial extraction should preserve the current AST visitor as one collector feeding the raw model. This reduces refactor risk while still creating the new phase boundaries.

Why:

- the existing structural fold coverage is valuable and should not be rewritten unnecessarily
- an adapter-style refactor is easier to verify against current tests than a full collector redesign

Alternative considered:

- Rewrite collection around a brand-new multi-source manager immediately. Rejected because it adds scope before the phase split is proven.

### 5. Move output shaping into an options-driven renderer

The renderer should translate normalized ranges into LSP folding ranges. Boundary shaping, output kinds, and optional metadata emission should live there, even if some options still use default values until later protocol plumbing exists.

Renderer input should be the normalized raw model plus a separate `FoldingRangeOptions` structure. The public feature API should follow the existing feature-options style, for example:

```cpp
struct FoldingRangeOptions {
    bool line_folding_only = false;
};

auto folding_ranges(CompilationUnitRef unit,
                    const FoldingRangeOptions& opts = {},
                    PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::FoldingRange>;
```

`line_folding_only` defaults to `false`, preserving the current behavior for existing callers. When server capability plumbing is added later, the server should translate `textDocument.foldingRange.lineFoldingOnly` into this option instead of exposing session state to collectors.

The renderer then becomes responsible for:

- converting `LocalSourceRange` into protocol positions for the requested encoding
- applying line-only adjustments when `opts.line_folding_only = true`
- mapping raw kind metadata to emitted LSP kinds
- deciding whether collapsed text is emitted or suppressed
- later applying deterministic `rangeLimit` trimming without changing collectors

This is the key point where `clice` should intentionally diverge from clangd. clangd threads `lineFoldingOnly` into collection and directly produces protocol objects. `clice` should keep those capability and transport decisions isolated in rendering so collectors remain stable as client support evolves.

Why:

- rendering rules are a separate concern from source discovery
- later work on line-only output, metadata gating, or public kind mapping should not force collector rewrites
- clangd-style line-only shaping is still supported, but as renderer policy rather than collector output
- isolating rendering makes behavioral diffs easier to review
- a small options object makes the missing `line_folding_only` support explicit without expanding `RawFoldingRange`

Alternative considered:

- Keep final boundary shaping next to the AST collector and only add a small helper for sorting. Rejected because it only moves a symptom, not the architectural problem.

## Risks / Trade-offs

- [Refactoring the current path can accidentally change fold ordering] -> Mitigation: add deterministic-order assertions and compare outputs for existing structural fixtures.
- [The raw model could become too abstract too early] -> Mitigation: do not redesign `RawFoldingRange` in this change; keep the existing fields unless implementation proves a concrete need.
- [Line-only behavior can be accidentally encoded in collectors] -> Mitigation: expose `line_folding_only` only through `FoldingRangeOptions` and assert renderer-level behavior in tests.
- [A renderer abstraction may appear premature before full capability plumbing exists] -> Mitigation: keep default render options aligned with current behavior and treat future options as extension points, not immediate scope.

## Migration Plan

1. Keep the existing `RawFoldingRange` collection path stable behind the current entrypoint.
2. Introduce `FoldingRangeOptions` with `line_folding_only = false` by default.
3. Insert normalization between collection and response emission.
4. Move LSP object construction into a dedicated renderer that consumes normalized ranges plus options.
5. Add line-only renderer tests and verify that existing structural folding fixtures still produce the expected default ranges.

Rollback strategy:

- If the refactor destabilizes output, keep the new helper types but temporarily route the old direct-emission path until normalization and rendering regressions are resolved.

## Open Questions

- Whether public kind remapping should land in this extracted change or remain a follow-up proposal once the renderer boundary exists.
- Whether `FoldingRangeOptions` should initially contain only `line_folding_only`, or also reserve fields for later collapsed-text and `rangeLimit` behavior.
