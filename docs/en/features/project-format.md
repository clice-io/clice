# Project-wide Format

## Overview

Beyond the LSP `textDocument/formatting` request (which formats a single open file), clice provides project-wide formatting via CLI.

**Usage**: `clice format` (not yet implemented — prints stub message)

## Current Status

- [x] Single-file formatting via LSP (see [formatting](./formatting.md))
- [ ] CLI `clice format` for batch formatting
- [ ] Parallel formatting across project files
- [ ] Incremental format (only modified files since last run)
- [ ] Dry-run / diff mode (show what would change)

## Implementation

Uses the same `clang::format` engine as LSP formatting. The CLI mode iterates over all source files referenced in the compilation database and applies format + include sorting.
