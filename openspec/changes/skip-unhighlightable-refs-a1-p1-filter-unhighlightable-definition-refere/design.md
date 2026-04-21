## Context

This proposal materializes `Filter Unhighlightable Definition References` for the canonical request `fix: skip unhighlightable definition references` (`fix`). clice does not currently expose a dedicated `textDocument/documentHighlight` feature, but both `feature::semantic_tokens` and `index::TUIndex` consume `SemanticVisitor::handleDeclOccurrence`, so permissive `NamedDecl` occurrence emission can surface as bogus highlightable ranges in multiple places.

`src/semantic/ast_utility.cpp` already centralizes `DeclarationName`-specific behavior such as identifier extraction and display-name formatting. That makes it the natural place to mirror clangd's `canHighlightName` switch and keep the semantic-token path and any index-backed highlight occurrence path aligned.

## Goals / Non-Goals

**Goals:**
- Add one shared helper that mirrors clangd's `canHighlightName(clang::DeclarationName)` acceptance rules.
- Stop emitting highlightable `NamedDecl` source occurrences for unsupported name kinds while preserving ordinary identifiers and constructors/destructors.
- Preserve symbol normalization, relation emission, and other navigation metadata that do not depend on highlightable source ranges.
- Add regression tests that lock both skipped and preserved cases.

**Non-Goals:**
- Add a new `textDocument/documentHighlight` implementation.
- Introduce special multi-token highlighting for operators or ObjC selectors in this change.
- Change symbol display names, hover text, completion labels, or USR generation.

## Decisions

### Add the helper in shared AST utilities

Place a `canHighlightName`-equivalent helper in `src/semantic/ast_utility.*`, next to the existing `DeclarationName` helpers. This keeps the accepted/rejected name-kind policy in one place and avoids duplicated switches across semantic-token and index consumers.

Alternative considered: duplicate the logic inside each consumer. Rejected because it would drift quickly and make regression fixes harder.

### Guard occurrence emission instead of relation emission

Apply the helper only where clice materializes highlightable `NamedDecl` source occurrences. The bug is about ranges that are exposed for highlighting/reference-style behavior, not about the relation graph itself, so `handleRelation` paths should continue to run even when no highlightable name range is emitted.

Alternative considered: guard all `SemanticVisitor::handleDeclOccurrence` forwarding in the base visitor. Rejected because it would silently change every current and future occurrence consumer and make navigation regressions harder to isolate.

### Keep TUIndex symbol and relation data intact

If TUIndex occurrence storage needs the same guard, the implementation should suppress only the occurrence records that represent highlightable ranges. Symbol creation, normalization, and relation insertion should still proceed so definition/reference, call hierarchy, and similar navigation features keep their backing metadata.

Alternative considered: skip whole symbols or relations for unhighlightable names. Rejected because the request is narrowly about highlightability, not symbol existence.

### Lock behavior with focused semantic-token and index tests

Semantic-token tests should cover anonymous or empty identifiers plus operator/conversion/literal names being skipped, and ordinary identifiers plus constructors/destructors still being emitted. TUIndex tests should verify skipped names do not appear as highlightable occurrence ranges and that unrelated relation data remains intact.

## Risks / Trade-offs

- [Filtering TUIndex occurrences can affect cursor resolution paths that rely on occurrence lookup] -> Mitigation: keep the guard scoped to highlightable occurrences, preserve relation data, and add index regression tests that cover unaffected identifier navigation.
- [Clang declaration-name kinds can drift from clangd's helper over time] -> Mitigation: mirror clangd's switch structure closely and encode accepted/rejected kinds in tests.
- [Constructors and destructors do not use ordinary identifier spelling] -> Mitigation: add explicit positive tests so the helper does not accidentally reject them during refactors.

## Migration Plan

No data migration is required. Rollout is the normal code-and-test path for this fix, and the TUIndex-side guard can be backed out independently if validation shows an unexpected navigation regression.

## Open Questions

None at proposal time. Implementation should confirm whether any direct cursor-resolution path needs separate handling for operator-like names beyond the scope of this fix.
