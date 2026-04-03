## 1. Semantic Token Collection

- [ ] 1.1 Audit `src/feature/semantic_tokens.cpp` against `SymbolKind` and `SymbolModifiers`, and add collection paths for the high-value AST-derived kinds/modifiers covered by the spec.
- [ ] 1.2 Replace equal-range `Conflict` collapsing with explicit token precedence and modifier-merging rules, and add focused unit tests for overlap behavior.
- [ ] 1.3 Extend token encoding tests to cover newly emitted semantic kinds/modifiers without regressing multiline and UTF-8/UTF-16 behavior.

## 2. C++20 Module Highlighting

- [ ] 2.1 Implement module-name token extraction for named module declarations using existing `CompilationUnitRef`, directive, and token-buffer APIs.
- [ ] 2.2 Implement module-name token extraction for `import` statements, including partition forms such as `Foo:Bar`.
- [ ] 2.3 Add regression tests for named modules, imports, global module fragments, private fragments, and dotted/partitioned module fixtures.

## 3. LSP Semantic Token Delivery

- [ ] 3.1 Extend server capability advertisement and request routing to support `textDocument/semanticTokens/full/delta`.
- [ ] 3.2 Add per-document semantic token caching with stable `resultId` handling and full-result fallback for stale delta requests.
- [ ] 3.3 Invalidate semantic token caches on document lifecycle changes and trigger semantic token refresh when recompilation changes highlighting for open documents.

## 4. Validation

- [ ] 4.1 Add or expand unit tests for operators, attributes, concepts, bracket-like tokens, and semantic modifiers that are newly emitted.
- [ ] 4.2 Add integration coverage for semantic token full and delta requests in the server test suite.
- [ ] 4.3 Run the relevant semantic token and module test targets, then fix any fixture or behavior mismatches discovered during verification.
