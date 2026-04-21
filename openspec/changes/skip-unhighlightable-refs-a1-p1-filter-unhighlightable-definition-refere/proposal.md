## Why

The canonical request `fix: skip unhighlightable definition references` (`fix`) targets a mismatch between clice and clangd: clice can emit highlight and reference occurrences for `NamedDecl`s whose `DeclarationName` has no highlightable source spelling. That leaks misleading ranges for anonymous declarations and special name kinds such as operators, conversions, and literal operators into the existing semantic-token and index-backed occurrence pipeline.

## What Changes

- Add a reusable helper equivalent to clangd's `canHighlightName(clang::DeclarationName)` that accepts non-empty identifiers plus constructors and destructors, and rejects anonymous or empty identifiers, ObjC selectors, conversion functions, overloaded operators, deduction guides, literal operators, and using-directive names.
- Apply that helper at the `NamedDecl` occurrence emission boundary used for highlightable source ranges so unhighlightable names stop producing highlight/reference occurrences while relation graphs and symbol metadata remain available for navigation.
- Add focused regression coverage for skipped anonymous/operator/conversion/literal cases and preserved identifier/constructor/destructor cases in semantic-token and TUIndex-oriented unit tests.

## Capabilities

### New Capabilities
- `skip-unhighlightable-refs-a1-p1-filter-unhighlightable-definition-refere`: Filter highlightable definition/reference occurrences to declaration names that have a concrete source spelling, matching the `skip-unhighlightable-definition-references` objective.

### Modified Capabilities
- None.

## Impact

- Shared semantic AST utilities in `src/semantic/ast_utility.*` gain the reusable declaration-name guard.
- Highlightable occurrence emitters in `src/feature/semantic_tokens.cpp` and, if required by the current occurrence pipeline, `src/index/tu_index.cpp` adopt the new guard without changing relation-only paths.
- Regression coverage is added in `tests/unit/feature/semantic_tokens_tests.cpp` and `tests/unit/index/tu_index_tests.cpp`, with validation centered on `pixi run unit-test Debug` and broader `pixi run test Debug` when feasible.
