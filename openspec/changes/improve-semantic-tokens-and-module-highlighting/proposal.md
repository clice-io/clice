## Why

`clice` already advertises semantic tokens, but the current implementation only covers a narrow subset of the available `SymbolKind` and modifier space. Compared with clangd, it is missing several high-value semantic classifications, has no protocol support for semantic token deltas or refresh, and leaves C++20 module names unhighlighted even though the semantic visitor already reserves hooks for them.

Improving this now will make highlighting materially more accurate in real C++ code, reduce the visible gap with clangd, and let `clice`'s existing C++20 module pipeline surface richer editor feedback instead of treating modules as plain identifiers.

## What Changes

- Expand semantic token collection beyond lexical tokens and basic declaration/reference handling so `clice` can emit more of its declared `SymbolKind` and modifier set.
- Add AST-driven classification for operators, bracket-like punctuation, attributes, concepts, and other symbols that currently fall back to plain lexical highlighting or are omitted entirely.
- Implement semantic highlighting for C++20 module declarations and imports, including named modules and partitions.
- Improve token conflict resolution so overlapping lexical and semantic classifications prefer the most specific semantic meaning instead of collapsing to `Conflict`.
- Add server-side support for semantic token result IDs and delta updates, with refresh support wired where document recompilation changes highlighting.
- Strengthen unit and integration coverage for semantic tokens, especially module-specific fixtures and multi-token semantic cases.

## Capabilities

### New Capabilities
- `semantic-tokens`: Provide richer semantic token classification for C++ code, including AST-derived symbol kinds, modifiers, incremental token delivery, and C++20 module-aware highlighting.

### Modified Capabilities
- None.

## Impact

- Affected code: `src/feature/semantic_tokens.cpp`, `src/semantic/semantic_visitor.h`, semantic token request handling in the server, and semantic token protocol wiring.
- Affected tests: `tests/unit/feature/semantic_tokens_tests.cpp`, server integration tests, and module fixtures under `tests/data/modules/`.
- User-visible behavior: editors will receive more accurate semantic token kinds/modifiers and module-aware highlighting without requiring API-breaking changes.
