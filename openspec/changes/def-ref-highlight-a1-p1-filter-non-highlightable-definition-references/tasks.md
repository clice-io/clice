# Filter non-highlightable definition references

- Canonical Request Title: `fix(highlight/references): filter non-highlightable definition references`
- Conventional Title: `fix(highlight/references)`

## 1. Shared Highlightability Metadata

- [ ] 1.1 Add a shared `clang::DeclarationName` helper in `src/semantic/ast_utility.*` that matches the requested clangd-style `canHighlightName` policy.
- [ ] 1.2 Extend `index::Symbol`, `src/index/schema.fbs`, and TU/project index serialization and merge paths so symbol metadata carries a persisted `highlightable_name` flag.
- [ ] 1.3 Populate the same flag in every indexing producer that builds symbol tables, including background TU indexing and open-file indexing.

## 2. Query Filtering

- [ ] 2.1 Update the highlight-oriented definition-origin reference query path to skip reference-highlight results when the resolved symbol is marked non-highlightable.
- [ ] 2.2 Keep stored relations and non-highlight query behavior unchanged outside that gated path.

## 3. Regression Coverage

- [ ] 3.1 Add unit tests that prove anonymous or operator-like definitions no longer yield indexed reference-highlight results.
- [ ] 3.2 Add positive unit tests that prove ordinary identifiers, constructors, and destructors still yield indexed reference-highlight results.
- [ ] 3.3 Extend integration coverage to verify the filter survives project-index persistence and open-file indexing.
