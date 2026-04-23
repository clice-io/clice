## 1. Existing Raw Model and Collector Boundary

- [x] 1.1 Treat the existing `RawFoldingRange` as the finished internal collection model for this change.
- [ ] 1.2 Keep the existing AST structural folding path routed through raw ranges instead of reintroducing direct collector-to-LSP emission.
- [ ] 1.3 Add regression fixtures or assertions that cover the currently supported structural fold categories before further rendering changes.

## 2. Normalization, Opts, and Rendering

- [ ] 2.1 Implement normalization for deterministic sorting, duplicate removal, and invalid-range filtering.
- [ ] 2.2 Introduce `FoldingRangeOptions`/`Opts` with `line_folding_only = false` as the default.
- [ ] 2.3 Introduce a dedicated renderer that converts normalized ranges plus `Opts` into final LSP folding-range objects.
- [ ] 2.4 Honor `line_folding_only` in rendering by shaping emitted boundaries for clients that only support whole-line folds.
- [ ] 2.5 Keep default rendered output compatible with current structural behavior while exposing extension points for future collectors and render rules.

## 3. Verification

- [ ] 3.1 Compare pre-refactor and post-refactor outputs for the existing structural folding test cases.
- [ ] 3.2 Add focused tests for `line_folding_only` output using the new folding options path.
- [ ] 3.3 Run relevant folding-range unit tests and fix any ordering, deduplication, or boundary regressions introduced by the new pipeline.
