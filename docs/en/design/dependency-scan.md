# Dependency Scanning

## Purpose

At startup, clice needs to know the include graph and module dependency map for the entire project -- before any file is opened or compiled. The dependency scanner (`scan_dependency_graph()`) builds this information by performing a fast BFS over all source files in the compilation database, scanning preprocessor directives with a lightweight lexer instead of running clang.

The resulting `DependencyGraph` is used for:

- **Header context resolution**: finding which source files transitively include a given header.
- **Staleness detection**: knowing which files depend on a changed file.
- **Module compilation ordering**: mapping module names to their interface unit files.

## Wave-Based BFS Algorithm

The scanner processes files in waves (breadth-first):

```
Wave 0:  All source files from the CDB
Wave 1:  Headers discovered via #include in wave 0
Wave 2:  Headers discovered via #include in wave 1
  ...
Wave N:  No new files discovered -> done
```

Each wave has three phases:

### Phase 1: Read + Scan (parallel)

All files in the current wave are dispatched to the libuv thread pool. Each worker thread reads the file content and runs a fast lexer (`scan()`) that extracts:

- `#include` / `#include_next` directives (with quoted/angled distinction).
- Whether each include is inside a conditional (`#if` / `#ifdef`).
- Module declarations (`export module X`) and module imports (`import X`).

The scanner creates a local event loop and uses `et::queue()` to run file I/O on worker threads, then `et::when_all()` to collect results.

### Phase 2: Include Resolution (single-threaded)

For each scanned file, the scanner resolves `#include` directives to actual file paths using the file's `SearchConfig` (extracted from CDB compile flags). Resolution uses a `ResolvedSearchConfig` that maps include search directories to pre-populated directory listings.

Resolved paths are interned into the `PathPool`. Newly discovered files that have not been scanned are added to the next wave.

### Phase 3: Graph Building

`DependencyGraph::set_includes()` records the resolved include edges, keyed by `(path_id, config_id)`. Module interface declarations are registered via `add_module()`. After all waves complete, `build_reverse_map()` constructs the reverse include map for upward BFS queries.

## Caching Strategy

`ScanCache` provides three caches that dramatically improve performance on warm runs (e.g. after a file save triggers a rescan):

### Directory Listing Cache (`DirListingCache`)

Maps directory paths to their file listings (the result of `readdir()`). On the first scan, all unique search directories from all configs are pre-populated in parallel on the thread pool, overlapped with Phase 1 of Wave 0. On warm runs, no `readdir()` calls are needed.

### Angled-Include Resolution Cache

Maps `(config_id, header_name)` to `(path_id, found_dir_idx)`. Angled includes (`#include <foo.h>`) resolve the same way regardless of which file includes them (given the same config), so results are cached and reused across all files sharing a config. The `ScanReport` tracks `include_cache_hits` to measure effectiveness.

### Scan Result Cache

Maps `path_id` to `ScanResult`. On warm runs, file I/O and lexer scanning are skipped entirely for cached files, making Phase 1 effectively free. Entries should be invalidated when a file changes on disk.

Additionally, `ScanCache` stores pre-computed `context_groups` (files grouped by unique compilation command), `configs` (per-group search configurations), and `initial_wave` (wave 0 entries), all of which are reused across successive scans.

## Output

The scanner produces:

- **DependencyGraph**: forward include edges per `(file, config)` pair, module name to path_id mappings, and (after `build_reverse_map()`) reverse include edges for upward traversal.
- **ScanReport**: detailed statistics including file counts (source vs. header), edge counts (conditional vs. unconditional), resolution accuracy, module count, per-wave timing breakdowns (Phase 1/2/3), cumulative I/O and lexer timing, filesystem call counts, and cache hit rates.

## Performance Characteristics

The scanner is designed for cold-start performance on large codebases:

- **Parallelism**: File reads and lexer scans run on the thread pool. Directory listing pre-population runs concurrently with Wave 0 scanning.
- **Prefetching**: During Phase 2 of wave N, newly discovered files are immediately queued for scanning on the thread pool. By the time wave N+1 starts, most scan tasks are already complete.
- **Minimal syscalls**: The directory listing cache and include resolution cache eliminate most filesystem operations after the first wave.
- **Per-wave statistics**: `ScanReport::WaveStats` records files processed, timing per phase, files discovered for the next wave, prefetch tasks launched, and cache hit counts -- enabling detailed cold-start analysis.

A typical scan processes thousands of files across 5-10 waves in under a second, with the majority of wall-clock time spent in Wave 0 (reading source files and populating the directory cache).
