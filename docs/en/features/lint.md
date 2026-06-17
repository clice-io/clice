# Lint

## Overview

clice integrates clang-tidy as a built-in linting engine. Unlike standalone clang-tidy which processes each TU independently, clice's architecture enables cross-TU coordination to eliminate redundant work.

**Usage**: `clice lint` (not yet implemented)

## Current Status

- [ ] Basic clang-tidy integration (single-TU, in-editor diagnostics)
- [ ] Project-wide lint via CLI (`clice lint`)
- [ ] Cross-TU header deduplication
- [ ] Incremental re-lint (only changed files)
- [ ] Lint result caching

## Cross-TU Optimization

### The Problem

clang-tidy processes each translation unit independently. A header included by N source files gets checked N times — multiplicative overhead that makes project-wide linting slow for large codebases.

### clice's Approach

As a persistent server with knowledge of the full compilation graph, clice can:

- [x] Track which headers are shared across TUs
- [ ] Hash declaration contents to skip re-checking identical declarations seen in prior TUs
- [ ] Schedule lint jobs with dependency awareness (lint shared headers once, propagate results)
- [ ] Cache per-header lint results keyed by content hash + check configuration
- [ ] Report deduplicated diagnostics (same warning in same header → show once)

### Expected Speedup

For a project with H shared headers and N TUs, standalone clang-tidy does O(N × H) work. With cross-TU dedup, clice reduces this to O(N + H) — each header is checked once regardless of how many TUs include it.

## clang-tidy Integration Quality

Issues that affect the quality of clang-tidy diagnostics within a language server:

- [ ] Suppress clang-tidy warnings from macros in system headers ([clangd#1587](https://github.com/clangd/clangd/issues/1587), [clangd#2000](https://github.com/clangd/clangd/issues/2000))
- [ ] Run checks on preprocessor directives in preamble (header guards, macros) ([clangd#2501](https://github.com/clangd/clangd/issues/2501), [clangd#160](https://github.com/clangd/clangd/issues/160))
- [ ] Configurable diagnostic severity per check category ([clangd#1937](https://github.com/clangd/clangd/issues/1937))
- [ ] Support loading clang-tidy plugins ([clangd#1458](https://github.com/clangd/clangd/issues/1458))
- [ ] Clang static analyzer support ([clangd#905](https://github.com/clangd/clangd/issues/905))
- [ ] Clean up replacements when applying clang-tidy fixes ([clangd#429](https://github.com/clangd/clangd/issues/429))
- [ ] Filter diagnostics by version control diff ([clangd#822](https://github.com/clangd/clangd/issues/822))

## Configuration

Respects standard `.clang-tidy` configuration files in the project tree.

## Changelog

| Date | Change                                         | PR  |
| ---- | ---------------------------------------------- | --- |
| —    | Stub implementation, dependency graph tracking | —   |
