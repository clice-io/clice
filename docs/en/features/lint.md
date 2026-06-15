# Lint

## Overview

clice integrates clang-tidy as a built-in linting engine. Unlike standalone clang-tidy which processes each TU independently, clice's architecture enables cross-TU coordination to eliminate redundant work.

**Usage**: `clice lint` (not yet implemented — prints stub message)

## Current Status

- [ ] Basic clang-tidy integration (single-TU, in-editor diagnostics) [^clice-90]
- [ ] Project-wide lint via CLI (`clice lint`)
- [ ] Cross-TU header deduplication
- [ ] Incremental re-lint (only changed files)
- [ ] Lint result caching

## Cross-TU Optimization

### The Problem

clang-tidy processes each translation unit independently. A header included by N source files gets checked N times — this is a multiplicative overhead that makes project-wide linting extremely slow for large codebases.

### clice's Approach

As a persistent server with knowledge of the full compilation graph, clice can:

- [x] Track which headers are shared across TUs (via `dep_graph`)
- [ ] Hash declaration contents to skip re-checking identical declarations seen in prior TUs
- [ ] Schedule lint jobs with dependency awareness (lint shared headers once, propagate results)
- [ ] Cache per-header lint results keyed by content hash + check configuration
- [ ] Report deduplicated diagnostics (same warning in same header → show once)

### Expected Speedup

For a project with H shared headers and N TUs, standalone clang-tidy does O(N × H) work. With cross-TU dedup, clice reduces this to O(N + H) — each header is checked once regardless of how many TUs include it.

## Configuration

```toml
# clice.toml
[project]
clang_tidy = true
```

Respects standard `.clang-tidy` configuration files in the project tree.

[^clice-90]: [clice#90](https://github.com/clice-io/clice/issues/90) — clang-tidy integration tracking issue `open`
