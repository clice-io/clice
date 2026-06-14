# Formatting

Implementation: `src/feature/formatting.cpp`

## Core

Uses `clang::format` library for all formatting operations.

- [x] Document formatting (`textDocument/formatting`)
- [x] Range formatting (`textDocument/rangeFormatting`)
- [x] Respect `.clang-format` style files
- [x] Include sorting (`clang::format::sortIncludes`)
- [x] Combined include sort + reformat in single pass

## Style Resolution

- [x] Auto-detect style from project `.clang-format`
- [x] Fallback to LLVM default style
- [ ] On-type formatting (`textDocument/onTypeFormatting`)
- [ ] Format-on-save integration
