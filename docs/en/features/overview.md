# Language Server Overview

clice provides a suite of C++ development tools built on LLVM/Clang. This section documents what's implemented, what's planned, and links to relevant upstream issues.

## Languages

clice serves C, C++ and CUDA, and Objective-C and Objective-C++ for reading code: semantic tokens, hover, navigation and references, call and type hierarchies, the outline, folding, workspace symbols and `clice query` know their classes, protocols, categories, methods, properties and instance variables, and rename takes classes, protocols, categories and instance variables. Renaming a method or a property and completing a message are not supported, nor are Clang header modules (`-fmodules`, `@import`): a file compiles without them, and clice says so once. Like every project, an Objective-C one needs a compilation database, which CMake's Makefile and Ninja generators write; neither Xcode nor CMake's Xcode generator does.

## Features

Language Server Protocol features available when using clice as an editor backend.

<!-- The status matrix counts the documented cases each feature page shows,
     in the order of the sidebar. Do not edit the region between the
     GENERATED markers by hand — run `pixi run update-feature-docs`. -->

<!-- BEGIN GENERATED OVERVIEW -->

| Feature                                   | Status                                   |
| ----------------------------------------- | ---------------------------------------- |
| [Code Completion](./completion.md)        | 49 supported                             |
| [Hover](./hover.md)                       | 72 supported                             |
| [Signature Help](./signature-help.md)     | 21 supported · 1 partial · 2 unsupported |
| [Code Navigation](./navigation.md)        | 115 supported · 8 unsupported            |
| [Rename](./rename.md)                     | 9 supported                              |
| [Diagnostics](./diagnostics.md)           | 10 supported · 1 partial                 |
| [Code Action](./code-action.md)           | 38 supported                             |
| [Semantic Tokens](./semantic-tokens.md)   | 61 supported                             |
| [Inlay Hints](./inlay-hints.md)           | 40 supported                             |
| [Document Links](./document-links.md)     | 5 supported · 1 partial                  |
| [Document Symbols](./document-symbols.md) | 29 supported                             |
| [Folding Ranges](./folding-ranges.md)     | 24 supported                             |
| [Selection Ranges](./selection-ranges.md) | 14 supported                             |
| [Formatting](./formatting.md)             | 4 supported                              |
| [C++20 Modules](./modules.md)             | 9 supported                              |
| [Header Contexts](./header-contexts.md)   | 6 supported                              |
| [Compilation](./compilation.md)           | 8 supported                              |

<!-- END GENERATED OVERVIEW -->

## Legend

Each feature counts the documented capabilities on its page at each status:

- **supported** — the capability works; a snapshot pins the behavior
- **partial** — incomplete; the snapshot pins what works today
- **unsupported** — a documented gap, tracked but not yet implemented
