# Formatting

## Core

- [x] Document formatting (`textDocument/formatting`)
- [x] Range formatting (`textDocument/rangeFormatting`)
- [x] Respect `.clang-format` style files
- [x] Include sorting
- [x] Combined include sort + reformat in single pass

## Style Resolution

- [x] Auto-detect style from `.clang-format` — clang-format searches parent directories from the source file up to the filesystem root
- [x] Fallback to LLVM default style when no `.clang-format` is found in any parent directory

## On-type and Save Hooks

- [ ] On-type formatting (`textDocument/onTypeFormatting`)
- [ ] Format-on-save integration

## Project-wide Format

Beyond the LSP `textDocument/formatting` request (which formats a single open file), clice provides project-wide formatting via CLI.

- [ ] CLI `clice format` for batch formatting
- [ ] Parallel formatting across project files
- [ ] Incremental format (only modified files since last run)
- [ ] Dry-run / diff mode (show what would change)

## Changelog

| Date | Change                                                 | PR  |
| ---- | ------------------------------------------------------ | --- |
| —    | Document formatting, range formatting, include sorting | —   |
