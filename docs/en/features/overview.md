# Language Server Overview

clice provides a suite of C++ development tools built on LLVM/Clang. This section documents what's implemented, what's planned, and links to relevant upstream issues.

## Features

Language Server Protocol features available when using clice as an editor backend.

<!-- The status matrix is generated from the snapshot fixtures under
     tests/snap/. Do not edit the region between the GENERATED markers by
     hand — edit the fixtures (or OVERVIEW_ROWS in tools/docs/feature.ts)
     and run `node tools/docs/feature.ts update`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| Feature                                   | Status                                     |
| ----------------------------------------- | ------------------------------------------ |
| [Code Completion](./completion.md)        | 31 supported                               |
| [Hover](./hover.md)                       | 34 supported · 21 partial · 11 unsupported |
| [Signature Help](./signature-help.md)     | 14 supported                               |
| [Code Navigation](./navigation.md)        | 44 supported · 14 partial · 34 unsupported |
| [Document Links](./document-links.md)     | 7 supported · 1 partial · 1 unsupported    |
| [Semantic Tokens](./semantic-tokens.md)   | 54 supported · 4 partial · 10 unsupported  |
| [Inlay Hints](./inlay-hints.md)           | 31 supported · 6 partial · 4 unsupported   |
| [Folding Ranges](./folding-ranges.md)     | 13 supported · 2 partial · 6 unsupported   |
| [Document Symbols](./document-symbols.md) | 18 supported · 2 partial · 7 unsupported   |
| [Formatting](./formatting.md)             | Implemented                                |
| [Diagnostics](./diagnostics.md)           | Partial                                    |
| [Code Action](./code-action.md)           | Stub                                       |

<!-- END GENERATED OVERVIEW -->

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
