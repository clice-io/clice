## Why

`clice`'s current `document_symbols` implementation collects `NamedDecl`s through the generic `FilteredASTVisitor`. Compared with `clangd`'s dedicated outline builder, it still lacks several important constraints: filtering function-local declarations, differentiating template specializations from instantiations, grouping macro-expanded declarations, and repairing `range` / `selectionRange` / parent-child nesting invariants.

These gaps directly affect outline stability and readability in editors, and they leave a visible behavior gap between `clice` and `clangd` on real C++ code. Closing them now should materially improve document outline quality without changing the LSP surface area.

## What Changes

- Replace the generic "collect interesting decls during recursive traversal" approach with an outline-specific traversal model.
- Exclude declarations that should not appear in document outline results, especially function-local declarations, implicit entities, and implicit template instantiations.
- Define template behavior explicitly so specializations, explicit instantiations, and ordinary template declarations appear in the outline in a source-intuitive way.
- Introduce macro container symbols for macro invocations written in the main file so expanded declarations appear under a stable hierarchy instead of leaking into the enclosing scope.
- Normalize symbol ordering and range invariants so `selectionRange` is always contained in `range`, and parent ranges always contain child ranges.
- Strengthen unit and integration coverage so regressions are caught by structural assertions instead of node counts alone.

## Capabilities

### New Capabilities
- `document-symbols`: Provide document outline output that matches source structure more closely and behaves robustly in template and macro-heavy code.

### Modified Capabilities
- None.

## Impact

- Affected code: `src/feature/document_symbols.cpp` and any reused helpers for macro locations or source range repair.
- Affected tests: `tests/unit/feature/document_symbol_tests.cpp` and `tests/integration/test_server.py`.
- User-visible behavior: editor Outline / Breadcrumb / Document Symbols views will behave more like `clangd`, especially for local declarations, templates, and macro-heavy code.
