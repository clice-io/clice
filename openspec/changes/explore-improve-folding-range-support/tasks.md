## 1. Reference Snapshot

- [x] 1.1 Download the clangd folding-range reference files for `llvmorg-21.1.8` into `openspec/changes/explore-improve-folding-range-support/reference/clangd/llvmorg-21.1.8/` using `curl` against GitHub raw URLs.
- [x] 1.2 Include `SemanticSelection.{cpp,h}`, `ClangdServer.{cpp,h}`, `ClangdLSPServer.cpp`, `Protocol.{h,cpp}`, `test/folding-range.test`, and `unittests/SemanticSelectionTests.cpp`.
- [x] 1.3 Record the exact raw GitHub URLs and downloaded file layout in a change-local comparison note.

## 2. Comparison and Pipeline

- [x] 2.1 Record a side-by-side comparison in the change artifacts between clangd's folding path and clice's current path, calling out confirmed parity gaps, clice-only capabilities, and known bugs.
- [ ] 2.2 Keep the existing `RawFoldingRange` model as the settled collection contract while completing normalization and options-driven rendering.
- [ ] 2.3 Replace direct exposure of clice-specific public folding kinds with a stable mapping to standard LSP `comment` / `imports` / `region` kinds.

## 3. Comment and Structural Baseline

- [ ] 3.1 Add a comment collector that folds multi-line block comments and contiguous multi-line `//` comment groups in the main file.
- [ ] 3.2 Preserve existing AST structural folding behavior while routing it through the new normalization/rendering pipeline.
- [ ] 3.3 Add focused unit tests for comment folding, single-line comment exclusion, and structural folding regressions.

## 4. Preprocessor Folding

- [ ] 4.1 Rework conditional-directive collection so each `#if/#elif/#else/#endif` branch body closes correctly, including the final branch ending at `#endif`.
- [ ] 4.2 Add folding support for inactive conditional branches using the existing preprocessor condition metadata.
- [ ] 4.3 Strengthen `#pragma region` handling and convert the current placeholder directive tests into assertion-backed coverage.

## 5. Protocol and Rendering

- [ ] 5.1 Capture client folding capabilities during initialize and translate them into `FoldingRangeOptions`/`Opts` when serving `textDocument/foldingRange`.
- [ ] 5.2 Honor `lineFoldingOnly` through `opts.line_folding_only`, gate `collapsedText`, and apply deterministic `rangeLimit` trimming during folding-range rendering.
- [ ] 5.3 Add integration coverage for line-only rendering, standard kind output, optional collapsed text, and range limiting.

## 6. Clice-Specific Folding Extensions

- [ ] 6.1 Add folding ranges for multi-line macro definitions using `directive.macros` and stable main-file source ranges.
- [ ] 6.2 Add grouping folds for contiguous `#include` blocks and return them as `imports` ranges.
- [ ] 6.3 Add grouping folds for contiguous C++ module `import` declarations and cover mixed include/import layouts with tests.

## 7. Verification

- [ ] 7.1 Run the relevant folding-range unit and integration tests, then fix any ordering, deduplication, or boundary regressions found during verification.
