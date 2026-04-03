## 1. Folding Range Pipeline

- [ ] 1.1 Refactor `src/feature/folding_ranges.cpp` so collection, normalization, and LSP rendering are separated by an internal raw-range model.
- [ ] 1.2 Add folding render options for `lineFoldingOnly`, `collapsedText` support, and optional `rangeLimit`, and thread them through the folding request path.
- [ ] 1.3 Replace direct exposure of clice-specific public folding kinds with a stable mapping to standard LSP `comment` / `imports` / `region` kinds.

## 2. Comment and Structural Baseline

- [ ] 2.1 Add a comment collector that folds multi-line block comments and contiguous multi-line `//` comment groups in the main file.
- [ ] 2.2 Preserve existing AST structural folding behavior while routing it through the new normalization/rendering pipeline.
- [ ] 2.3 Add focused unit tests for comment folding, single-line comment exclusion, and structural folding regressions.

## 3. Preprocessor Folding

- [ ] 3.1 Rework conditional-directive collection so each `#if/#elif/#else/#endif` branch body closes correctly, including the final branch ending at `#endif`.
- [ ] 3.2 Add folding support for inactive conditional branches using the existing preprocessor condition metadata.
- [ ] 3.3 Strengthen `#pragma region` handling and add assertion-based tests for nested region folding.

## 4. Clice-Specific Folding Extensions

- [ ] 4.1 Add folding ranges for multi-line macro definitions using `directive.macros` and stable main-file source ranges.
- [ ] 4.2 Add grouping folds for contiguous `#include` blocks and return them as `imports` ranges.
- [ ] 4.3 Add grouping folds for contiguous C++ module `import` declarations and cover mixed include/import layouts with tests.

## 5. Protocol and Validation

- [ ] 5.1 Capture client folding capabilities during initialize and use them when serving `textDocument/foldingRange`.
- [ ] 5.2 Add integration coverage for `lineFoldingOnly`, `collapsedText` gating, and kind/output compatibility.
- [ ] 5.3 Run the relevant folding-range unit and integration tests, then fix any ordering, deduplication, or boundary regressions found during verification.
