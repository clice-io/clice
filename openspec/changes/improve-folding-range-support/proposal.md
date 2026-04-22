## Why

`clice` already goes beyond clangd in several structural folding cases: it can fold namespaces, records, function parameter lists and bodies, lambda captures, call argument lists, access-specifier sections, and some preprocessor regions. However, the current implementation in `src/Feature/FoldingRange.cpp` still misses several baseline behaviors that clangd already exposes well, especially multiline comment folding, line-only folding rendering, standard public folding kinds, and a stronger regression test matrix. Its preprocessor branch folding is also not yet fully closed.

This branch is also explicitly about measuring the gap against a fixed upstream reference, not against memory. At tag `llvmorg-21.1.8`, clangd's folding implementation is centered around `clang-tools-extra/clangd/SemanticSelection.cpp`, with request plumbing in `ClangdServer.cpp` and `ClangdLSPServer.cpp`, protocol types in `Protocol.h` and `Protocol.cpp`, and folding coverage in `test/folding-range.test` plus folding-related unit tests. Vendoring that source into the workspace first gives the branch a stable baseline for side-by-side comparison and later commits.

More importantly, `clice` already has preprocessor metadata that clangd does not fully exploit, such as `directive.macros`, `directive.includes`, `directive.imports`, and evaluated conditional-branch state. That means `clice` should not stop at matching clangd: after filling the real parity gaps, folding ranges can become a more useful C/C++ feature by covering macro definitions, `#if` branches, and include/import groups that clangd does not currently handle well.

## What Changes

- Vendor a focused clangd folding-range reference snapshot from tag `llvmorg-21.1.8` into `third_party/clangd/llvmorg-21.1.8/` using `curl` from GitHub raw URLs, then commit that snapshot separately so the comparison baseline is explicit.
- Record a concrete clangd-vs-clice comparison in the tracked change, including the upstream files consulted, confirmed parity gaps, clice-only capabilities, and known implementation bugs.
- Fill the remaining folding-range baseline gaps between `clice` and clangd, especially multiline comment folding, line-only folding rendering, and standard public kind mapping.
- Complete preprocessor-related folding so full `#if/#elif/#else/#endif` branch regions, nested `#pragma region` blocks, and inactive branches have well-defined behavior.
- Add folding features that take advantage of `clice`'s existing preprocessor metadata, including multiline macro definitions and grouped `#include` / `import` blocks.
- Normalize `FoldingRange.kind` output so standard kinds remain compatible while clice-specific fold categories degrade predictably.
- Make folding range responses honor client capabilities such as `lineFoldingOnly`, optional `collapsedText` support, and range limiting.
- Expand unit and integration coverage for AST folds, comments, preprocessor regions, macros, include/import groups, and protocol negotiation behavior.

## Capabilities

### New Capabilities
- `folding-ranges`: Provide LSP-compatible, C/C++-focused folding regions that cover AST structure, comments, preprocessor branches, macro definitions, and include/import groups.

### Modified Capabilities
- None.

## Impact

- A vendored comparison snapshot will be added under `third_party/clangd/llvmorg-21.1.8/`, limited to the folding-range implementation, protocol, and test files needed for analysis.
- Primary runtime impact is in `src/Feature/FoldingRange.cpp`, the compile-unit/preprocessor metadata access paths, request handling in `src/Server/Feature.cpp`, and capability negotiation around `src/Server/Lifecycle.cpp`.
- Tests need expansion in `tests/unit/Feature/FoldingRangeTests.cpp`, server/integration coverage, and any required fixtures for preprocessor and module scenarios.
- User-visible behavior will be folding results that are closer to clangd where clangd already has coverage, while also adding high-value C/C++ folds that clangd does not currently provide well, especially macro-definition and conditional-compilation folding.
