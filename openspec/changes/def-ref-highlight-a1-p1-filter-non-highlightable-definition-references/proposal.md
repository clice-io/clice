# Filter non-highlightable definition references

- Canonical Request Title: `fix(highlight/references): filter non-highlightable definition references`
- Conventional Title: `fix(highlight/references)`

## Why

Definition-origin reference highlighting currently trusts indexed symbol metadata without checking whether the originating declaration name can actually be highlighted in source. That creates noisy or impossible highlight results for anonymous and operator-like definitions, and the decision cannot be recovered at query time unless it is preserved in the index.

## What Changes

- Add a shared `clang::DeclarationName` predicate that mirrors clangd-style `canHighlightName` behavior for identifiers, constructors, destructors, and rejected name kinds.
- Persist a `highlightable_name` flag through TU index construction, FlatBuffer serialization, project-index merge/load/save, and open-file symbol metadata.
- Apply the filter only in the definition-origin reference-highlighting query path so non-highlight relation data stays intact.
- Add regression coverage for negative cases such as anonymous or operator-like definitions and positive cases such as ordinary identifiers, constructors, and destructors.

## Capabilities

### New Capabilities

- `def-ref-highlight-a1-p1-filter-non-highlightable-definition-references`: Definition-origin reference highlighting uses persisted symbol metadata to suppress non-highlightable definition names while preserving valid ones.

### Modified Capabilities

None.

## Impact

- Affected code: `src/semantic/ast_utility.*`, `src/index/tu_index.*`, `src/index/schema.fbs`, `src/index/project_index.*`, and the index-backed relation query path in `src/server/`.
- Affected behavior: indexed reference highlighting for definition-origin queries now matches clangd-style highlightable-name eligibility.
- Verification: unit coverage in `tests/unit/index/index_query_tests.cpp` and integration coverage in `tests/integration/features/test_index.py` or an equivalent focused fixture.
