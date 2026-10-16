# Language Server Overview

clice provides a suite of C++ development tools built on LLVM/Clang. This section documents what's implemented, what's planned, and links to relevant upstream issues.

## Features

Language Server Protocol features available when using clice as an editor backend.

<!-- The status matrix counts the documented cases each feature page shows,
     in the order of the sidebar. Do not edit the region between the
     GENERATED markers by hand — run `pixi run update-feature-docs`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| Feature                                   | Status                                    |
| ----------------------------------------- | ----------------------------------------- |
| [Code Completion](./completion.md)        | 45 supported                              |
| [Hover](./hover.md)                       | 64 supported                              |
| [Signature Help](./signature-help.md)     | 18 supported · 1 partial · 3 unsupported  |
| [Code Navigation](./navigation.md)        | 96 supported · 2 partial · 12 unsupported |
| [Rename](./rename.md)                     | 4 supported                               |
| [Diagnostics](./diagnostics.md)           | 10 supported                              |
| [Code Action](./code-action.md)           | 38 supported                              |
| [Semantic Tokens](./semantic-tokens.md)   | 40 supported · 2 partial · 5 unsupported  |
| [Inlay Hints](./inlay-hints.md)           | 35 supported                              |
| [Document Links](./document-links.md)     | 7 supported · 1 partial                   |
| [Document Symbols](./document-symbols.md) | 29 supported                              |
| [Folding Ranges](./folding-ranges.md)     | 24 supported                              |
| [Selection Ranges](./selection-ranges.md) | 14 supported                              |
| [Formatting](./formatting.md)             | 4 supported                               |
| [C++20 Modules](./modules.md)             | 7 supported                               |
| [Header Contexts](./header-contexts.md)   | 5 supported                               |
| [Compilation](./compilation.md)           | 5 supported · 1 partial                   |

<!-- END GENERATED OVERVIEW -->

## Legend

Each feature counts the documented capabilities on its page at each status:

- **supported** — the capability works; a snapshot pins the behavior
- **partial** — incomplete; the snapshot pins what works today
- **unsupported** — a documented gap, tracked but not yet implemented
