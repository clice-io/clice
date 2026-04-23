## 1. Raw Model and Collector Boundary

- [ ] 1.1 Introduce internal raw folding-range and render-option types while keeping the current folding entrypoint stable.
- [ ] 1.2 Convert the existing AST structural folding path in `src/feature/folding_ranges.cpp` to emit raw ranges instead of final LSP ranges.
- [ ] 1.3 Add regression fixtures or assertions that cover the currently supported structural fold categories before further refactoring.

## 2. Normalization and Rendering

- [ ] 2.1 Implement normalization for deterministic sorting, duplicate removal, and invalid-range filtering.
- [ ] 2.2 Introduce a dedicated renderer that converts normalized ranges into final LSP folding-range objects.
- [ ] 2.3 Keep default rendered output compatible with current structural behavior while exposing extension points for future collectors and render rules.

## 3. Verification

- [ ] 3.1 Compare pre-refactor and post-refactor outputs for the existing structural folding test cases.
- [ ] 3.2 Run relevant folding-range unit tests and fix any ordering, deduplication, or boundary regressions introduced by the new pipeline.
