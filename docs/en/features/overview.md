# Features Overview

clice provides a suite of C++ development tools built on LLVM/Clang. This section documents what's implemented, what's planned, and links to relevant upstream issues.

## LSP Editor Features

Language Server Protocol features available when using clice as an editor backend.

<!-- The status matrix is generated from the snapshot fixtures under
     tests/snap/. Do not edit the region between the GENERATED markers by
     hand — edit the fixtures (or OVERVIEW_ROWS in tools/feature_docs.ts)
     and run `node tools/feature_docs.ts update`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| Feature          | Status                                     | Page                                      |
| ---------------- | ------------------------------------------ | ----------------------------------------- |
| Code Completion  | 30 supported                               | [completion](./completion.md)             |
| Hover            | 34 supported · 21 partial · 11 unsupported | [hover](./hover.md)                       |
| Signature Help   | 14 supported                               | [signature-help](./signature-help.md)     |
| Code Navigation  | 44 supported · 14 partial · 34 unsupported | [navigation](./navigation.md)             |
| Document Links   | 7 supported · 1 partial · 1 unsupported    | [document-links](./document-links.md)     |
| Semantic Tokens  | 52 supported · 4 partial · 10 unsupported  | [semantic-tokens](./semantic-tokens.md)   |
| Inlay Hints      | 31 supported · 6 partial · 4 unsupported   | [inlay-hints](./inlay-hints.md)           |
| Folding Ranges   | 13 supported · 2 partial · 6 unsupported   | [folding-ranges](./folding-ranges.md)     |
| Document Symbols | 18 supported · 2 partial · 7 unsupported   | [document-symbols](./document-symbols.md) |
| Formatting       | Implemented                                | [formatting](./formatting.md)             |
| Diagnostics      | Partial                                    | [diagnostics](./diagnostics.md)           |
| Code Action      | Stub                                       | [code-action](./code-action.md)           |

<!-- END GENERATED OVERVIEW -->

## Lint

Project-wide static analysis powered by clang-tidy, with cross-TU optimizations unique to clice.

| Feature                | Status  | Page              |
| ---------------------- | ------- | ----------------- |
| clang-tidy integration | Planned | [lint](./lint.md) |

## Legend

Fixture-backed features count the documented capabilities their test corpus pins at each status:

- **supported** — the capability works; a snapshot pins the behavior
- **partial** — incomplete; the snapshot pins what works today
- **unsupported** — a documented gap, tracked but not yet implemented

Features not yet on the fixture pipeline keep a hand-assigned label:

- **Implemented** — core functionality working, minor gaps only
- **Partial** — key subsystems missing (e.g., module support)
- **Stub** — handler exists but returns empty/null
- **Planned** — designed but not yet implemented
