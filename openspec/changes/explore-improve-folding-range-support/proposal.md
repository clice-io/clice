## Why

`clice` already goes beyond clangd in several structural folding cases: it can fold namespaces, records, function parameter lists and bodies, lambda captures, call argument lists, access-specifier sections, and some preprocessor regions. However, the current implementation in `src/feature/folding_ranges.cpp` still misses several baseline behaviors that clangd already exposes well, especially multiline comment folding, line-only folding rendering, standard public folding kinds, and a stronger regression test matrix. Its preprocessor branch folding is also not yet fully closed.

This exploration branch needs a fixed upstream reference instead of relying on memory. At tag `llvmorg-21.1.8`, clangd's folding implementation is centered around `clang-tools-extra/clangd/SemanticSelection.cpp`, with request plumbing in `ClangdServer.cpp` and `ClangdLSPServer.cpp`, protocol types in `Protocol.h` and `Protocol.cpp`, and folding coverage in `test/folding-range.test` plus `unittests/SemanticSelectionTests.cpp`. Downloading those files into this change directory with `curl` gives the branch a stable, reviewable baseline for side-by-side comparison without introducing a repository-level vendor tree.

More importantly, `clice` already has preprocessor metadata that clangd does not fully exploit, such as `directive.macros`, `directive.includes`, `directive.imports`, and evaluated conditional-branch state. That means `clice` should not stop at matching clangd: after filling the real parity gaps, folding ranges can become a more useful C/C++ feature by covering macro definitions, `#if` branches, and include/import groups that clangd does not currently handle well.

Follow-up discussion clarified the split with `split-folding-range-pipeline`: the existing `RawFoldingRange` model is finished for the current architecture work. The missing capability path is explicit folding options, passed as `Opts`/`FoldingRangeOptions`, so `line_folding_only` can be requested by server capability plumbing and consumed by the renderer.

## What Changes

- Download the clangd folding-range reference files for tag `llvmorg-21.1.8` into `openspec/changes/explore-improve-folding-range-support/reference/clangd/llvmorg-21.1.8/` using `curl` from GitHub raw URLs.
- Record a concrete clangd-vs-clice comparison in `comparison.md`, including the upstream files consulted, exact download URLs, confirmed parity gaps, clice-only capabilities, and known implementation bugs.
- Fill the remaining folding-range baseline gaps between `clice` and clangd, especially multiline comment folding, line-only folding rendering, and standard public kind mapping.
- Complete preprocessor-related folding so full `#if/#elif/#else/#endif` branch regions, nested `#pragma region` blocks, and inactive branches have well-defined behavior.
- Add folding features that take advantage of `clice`'s existing preprocessor metadata, including multiline macro definitions and grouped `#include` / `import` blocks.
- Normalize `FoldingRange.kind` output so standard kinds remain compatible while clice-specific fold categories degrade predictably.
- Make folding range responses honor client capabilities such as `lineFoldingOnly`, optional `collapsedText` support, and range limiting, using folding options rather than collector-specific state.
- Expand unit and integration coverage for AST folds, comments, preprocessor regions, macros, include/import groups, and protocol negotiation behavior.

## Capabilities

### New Capabilities
- `folding-ranges`: Provide LSP-compatible, C/C++-focused folding regions that cover AST structure, comments, preprocessor branches, macro definitions, and include/import groups.

### Modified Capabilities
- None.

## Impact

- A change-local upstream reference set has been downloaded under `openspec/changes/explore-improve-folding-range-support/reference/clangd/llvmorg-21.1.8/`, limited to the folding-range implementation, protocol, and tests needed for analysis.
- The side-by-side analysis is recorded in `openspec/changes/explore-improve-folding-range-support/comparison.md`.
- Primary runtime impact is in `src/feature/folding_ranges.cpp`, the compile-unit/preprocessor metadata access paths, request handling in `src/server/master_server.cpp`, and the folding options object used to carry capability-derived rendering choices.
- Tests need expansion in `tests/unit/feature/folding_range_tests.cpp`, server/integration coverage, and any required fixtures for preprocessor and module scenarios.
- User-visible behavior will be folding results that are closer to clangd where clangd already has coverage, while also adding high-value C/C++ folds that clangd does not currently provide well, especially macro-definition and conditional-compilation folding.
