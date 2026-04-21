## Request Metadata

- Proposal Title: Filter semantic highlight references by name eligibility
- Canonical Request Title: `fix(semantic-tokens): filter ineligible highlight references`
- Conventional Title: `fix(semantic-tokens)`

## Why

`SemanticTokensCollector::handleDeclOccurrence()` currently emits reference tokens for every referenced `NamedDecl`, even when the declaration name cannot be highlighted reliably from source text. Mirroring clangd's `canHighlightName` rule in this path fixes unsupported reference highlights without broadening the change into references, indexing, or a new document-highlight feature.

## What Changes

- Add a reusable declaration-name eligibility helper in `src/semantic/ast_utility.{h,cpp}` that mirrors clangd's `canHighlightName` behavior for `clang::DeclarationName`.
- Apply the helper in `src/feature/semantic_tokens.cpp` so reference semantic tokens are skipped when the target declaration name is not source-highlightable.
- Keep declaration and definition token behavior unchanged unless implementation evidence shows the failing repro also comes from those paths.
- Add focused semantic-token regression coverage for one unsupported-name reference case and one constructor/destructor positive case.

## Capabilities

### New Capabilities
- `highlight-name-filter-a1-p1-filter-semantic-highlight-references-by-name`: Semantic token reference highlighting only emits reference tokens for declaration names that are eligible for source highlighting, while constructors and destructors remain highlighted.

### Modified Capabilities
None.

## Impact

- Affected code: `src/feature/semantic_tokens.cpp`, `src/semantic/ast_utility.h`, `src/semantic/ast_utility.cpp`, and `tests/unit/feature/semantic_tokens_tests.cpp`
- Affected behavior: semantic-token highlighting for reference occurrences only
- Out of scope: `textDocument/references`, index serialization, symbol naming, and workspace-symbol search
- Dependencies: no new third-party dependencies
