## Context

This change extracts decision `2` from `openspec/changes/explore-improve-folding-range-support/design.md` into a standalone proposal. The current folding implementation in `src/feature/folding_ranges.cpp` mixes three responsibilities in one path:

- discovering foldable structure from AST data
- deciding which ranges survive deduplication and validation
- shaping the final LSP response, including output metadata

That coupling makes the code harder to extend safely. Comment folding, directive-based collectors, capability-aware rendering, and range limiting all become riskier when collection and rendering rules share the same code path. The extracted proposal keeps scope narrower: it does not add new fold categories by itself, but it creates the architecture that later changes can build on without destabilizing existing structural folding.

The downloaded clangd reference confirms both the value and the limit of the upstream design. clangd has useful, tested folding behavior for brace bodies, comment blocks, contiguous `//` groups, and `lineFoldingOnly`, but its implementation largely emits protocol-shaped `FoldingRange` objects directly from collection code. In `SemanticSelection.cpp`, both the AST path and the pseudo-parser path build `FoldingRange` results directly, and the pseudo-parser applies rendering details such as delimiter trimming and `lineFoldingOnly` adjustments while collecting ranges. That is a good behavior reference, but it is not the architecture this extracted change should copy.

`clice` already has stronger ingredients for a real pipeline:

- `LocalSourceRange` gives us a main-file, half-open offset representation that is independent of LSP position encoding
- directive metadata already captures information clangd does not expose well, including conditional-branch state, pragma regions, includes, imports, and macro references
- the current tests are boundary-oriented, which makes them a good fit for validating raw spans before protocol rendering

The design therefore separates "what fold exists in the source" from "how that fold should be emitted to this client". clangd's tested boundary rules are still relevant, but they should become renderer policy and normalization rules rather than collector output format.

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

### 1. Use clangd as a behavior reference, not an architecture template

This change should borrow clangd's confirmed folding behavior where it is useful, especially around multiline comments, contiguous `//` comment groups, main-file-only filtering, and `lineFoldingOnly` boundary shaping. It should not copy clangd's habit of emitting protocol-shaped `FoldingRange` objects directly from collection logic.

Why:

- clangd's tests are valuable because they pin down tricky folding behavior around comments, macro boundaries, and line-only rendering
- clangd's data flow is intentionally narrow and mixes collection with response shaping
- `clice` already has richer file-local and directive metadata that supports a cleaner internal representation

Alternative considered:

- Treat clangd's direct `FoldingRange` construction as the architecture to reproduce. Rejected because it would preserve the same coupling this extracted change is meant to remove.

### 2. Introduce a raw internal folding-range model

Collectors should emit an internal `RawFoldingRange`-style structure instead of final LSP protocol objects. The raw model should preserve source locations, an internal category, and optional metadata hints that later phases may use.

The raw model should be shaped around file-local source structure, not LSP transport fields. At minimum it should carry:

- a main-file `LocalSourceRange` span using half-open byte offsets
- an internal fold category such as namespace, record, access section, function body, comment block, comment group, conditional branch, pragma region, include group, or import group
- the collector origin, such as AST, comment scanning, or directive metadata, so normalization has a stable tie-break and debugging surface
- render hints for syntax-specific shaping, such as delimiter trimming, whether line-only folding should hide the final line, and an optional collapsed-text hint

In other words, the raw model should look closer to:

```cpp
struct RawFoldRenderHint {
    std::uint8_t trim_start_bytes = 0;
    std::uint8_t trim_end_bytes = 0;
    bool hide_last_line_when_line_only = false;
    std::string collapsed_text_hint;
};

struct RawFoldingRange {
    LocalSourceRange span;
    RawFoldCategory category;
    RawFoldOrigin origin;
    RawFoldRenderHint render;
};
```

The important design choice is that `span` represents the foldable source envelope in the main file, while renderer-specific trimming stays in `render` hints. For example:

- brace-based structural folds keep the full braced span and let the renderer trim interior boundaries
- block comments keep the full `/* ... */` span and let the renderer decide whether to hide the closing delimiter or final line
- contiguous `//` groups keep the full grouped span and let the renderer decide how much of the opening sentinel remains visible

Why:

- collectors should describe what was found, not how it will be serialized
- `LocalSourceRange` is already the natural coordinate system for `clice`
- public LSP kinds such as `comment`, `imports`, and `region` are too lossy to use as the internal category model
- future comment and directive collectors can share the same pipeline contract
- tests can validate collection independently from rendering

Alternatives considered:

- Continue emitting LSP ranges directly from collectors. Rejected because it keeps protocol concerns entangled with source discovery.
- Make the raw model store already-trimmed visible interior spans instead of the full source envelope. Rejected because line-only rendering, collapsed text, and comment delimiter rules would still leak back into every collector.

### 3. Normalize ranges before rendering

All collected ranges should pass through a normalization step before any response is emitted. Normalization is responsible for deterministic ordering, duplicate removal, and rejection of degenerate or unmappable ranges.

Normalization should operate on raw spans and internal categories, not on final LSP fields. Its responsibilities include:

- deterministic ordering independent of collector traversal order
- duplicate collapse for collectors that discover the same fold
- invalid-range filtering after raw spans and render hints are reconciled
- stable tie-breaking for overlapping ranges from different origins

Collectors may still reject obviously invalid inputs, such as non-main-file locations that cannot be mapped to `LocalSourceRange`, but normalization remains the phase that decides which collected folds survive to rendering.

Why:

- duplicate or invalid ranges are easier to reason about in one place than across many collectors
- stable ordering reduces regression noise and makes range limiting predictable later
- category-aware normalization preserves internal meaning until the renderer maps it to public kinds
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

### 5. Move output shaping into a dedicated renderer

The renderer should translate normalized ranges into LSP folding ranges. Boundary shaping, output kinds, and optional metadata emission should live there, even if some options still use default values until later protocol plumbing exists.

Renderer input should be the normalized raw model plus a separate `FoldingRenderOptions` structure. The renderer then becomes responsible for:

- converting `LocalSourceRange` into protocol positions for the requested encoding
- applying delimiter trimming and line-only adjustments
- mapping internal categories to public LSP kinds
- deciding whether collapsed text is emitted or suppressed
- later applying deterministic `rangeLimit` trimming without changing collectors

This is the key point where `clice` should intentionally diverge from clangd. clangd threads `lineFoldingOnly` into collection and directly produces protocol objects. `clice` should keep those capability and transport decisions isolated in rendering so collectors remain stable as client support evolves.

Why:

- rendering rules are a separate concern from source discovery
- later work on line-only output, metadata gating, or public kind mapping should not force collector rewrites
- clangd-style line-only shaping is still supported, but as renderer policy rather than collector output
- isolating rendering makes behavioral diffs easier to review

Alternative considered:

- Keep final boundary shaping next to the AST collector and only add a small helper for sorting. Rejected because it only moves a symptom, not the architectural problem.

## Risks / Trade-offs

- [Refactoring the current path can accidentally change fold ordering] -> Mitigation: add deterministic-order assertions and compare outputs for existing structural fixtures.
- [The raw model could become too abstract too early] -> Mitigation: keep the initial fields minimal and only include data already needed by current structural folds.
- [Full-envelope raw spans plus render hints may feel less direct than storing already-trimmed ranges] -> Mitigation: use a small, explicit render-hint structure and validate brace/comment shaping with focused renderer tests.
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
- Whether collector origin should remain part of the long-term raw model after normalization policy stabilizes, or only exist temporarily as a debugging and tie-break aid.
