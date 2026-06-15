# Features Overview

clice provides a suite of C++ development tools built on LLVM/Clang. This section documents what's implemented, what's planned, and links to relevant upstream issues.

## LSP Editor Features

Language Server Protocol features available when using clice as an editor backend.

| Feature          | Status      | Page                                      |
| ---------------- | ----------- | ----------------------------------------- |
| Code Completion  | Partial     | [completion](./completion.md)             |
| Hover            | Implemented | [hover](./hover.md)                       |
| Signature Help   | Implemented | [signature-help](./signature-help.md)     |
| Go to Definition | Partial     | [navigation](./navigation.md)             |
| Document Links   | Partial     | [document-links](./document-links.md)     |
| Semantic Tokens  | Implemented | [semantic-tokens](./semantic-tokens.md)   |
| Inlay Hints      | Implemented | [inlay-hints](./inlay-hints.md)           |
| Folding Ranges   | Implemented | [folding-ranges](./folding-ranges.md)     |
| Document Symbols | Implemented | [document-symbols](./document-symbols.md) |
| Formatting       | Implemented | [formatting](./formatting.md)             |
| Diagnostics      | Partial     | [diagnostics](./diagnostics.md)           |

## Lint

Project-wide static analysis powered by clang-tidy, with cross-TU optimizations unique to clice.

| Feature                | Status  | Page              |
| ---------------------- | ------- | ----------------- |
| clang-tidy integration | Planned | [lint](./lint.md) |

## Format

Code formatting capabilities beyond the LSP `textDocument/formatting` request.

| Feature                              | Status  | Page                                  |
| ------------------------------------ | ------- | ------------------------------------- |
| Project-wide format (`clice format`) | Planned | [project-format](./project-format.md) |

## Legend

- **Implemented** — core functionality working, minor gaps only
- **Partial** — key subsystems missing (e.g., module support)
- **Stub** — handler exists but returns empty/null
- **Planned** — designed but not yet implemented
