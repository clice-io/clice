## Why

`clice`'s folding range support is already more aggressive than clangd's: it can fold namespaces, records, function parameter lists and bodies, lambda captures, call argument lists, access-specifier sections, and some preprocessor regions. However, it still lacks several useful baseline capabilities that clangd already has, especially multiline comment folding, client capability negotiation, and a stronger regression test matrix. Its preprocessor branch folding is also not yet fully closed.

More importantly, `clice` already has preprocessor metadata that clangd does not fully exploit, such as `directive.macros`, `directive.includes`, `directive.imports`, and evaluated conditional-branch state. That means `clice` should not stop at matching clangd: folding ranges can become a feature that is more useful for real-world C/C++ editing by covering macro definitions, `#if` branches, and include/import groups that clangd does not currently handle well.

## What Changes

- Fill the remaining folding-range baseline gaps between `clice` and clangd, especially multiline comment folding and protocol-level client capability handling.
- Complete preprocessor-related folding so full `#if/#elif/#else/#endif` branch regions, nested `#pragma region` blocks, and inactive branches have well-defined behavior.
- Add folding features that take advantage of `clice`'s existing preprocessor metadata, including multiline macro definitions and grouped `#include` / `import` blocks.
- Normalize `FoldingRange.kind` and `collapsedText` output so standard kinds remain compatible while clice-specific fold categories degrade predictably.
- Make folding range responses honor client capabilities such as `lineFoldingOnly`, `collapsedText` support, and range limiting.
- Expand unit and integration coverage for AST folds, comments, preprocessor regions, macros, include/import groups, and protocol negotiation behavior.

## Capabilities

### New Capabilities
- `folding-ranges`: Provide LSP-compatible, C/C++-focused folding regions that cover AST structure, comments, preprocessor branches, macro definitions, and include/import groups.

### Modified Capabilities
- None.

## Impact

- Primary impact is in `src/feature/folding_ranges.cpp`, the compile-unit/preprocessor metadata access paths, and folding range request handling plus capability negotiation around `src/server/master_server.cpp`.
- Tests need expansion in `tests/unit/feature/folding_range_tests.cpp`, server/integration coverage, and any required fixtures for preprocessor and module scenarios.
- User-visible behavior will be folding results that are closer to clangd where clangd already has coverage, while also adding high-value C/C++ folds that clangd does not currently provide well, especially macro-definition and conditional-compilation folding.
