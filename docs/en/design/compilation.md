# Compilation

## Pull-Based Compilation Model

clice uses a **pull-based** compilation model. Document events (`didOpen`, `didChange`) only update Session state -- they never trigger compilation directly. Instead, compilation is triggered lazily by feature requests (hover, semantic tokens, go-to-definition, etc.), which call `Compiler::ensure_compiled()` before executing.

This design avoids wasted work: if the user types rapidly, only the final state is compiled when a feature request arrives.

```
didOpen  / didChange  -->  update Session.text, increment generation, set ast_dirty
didSave               -->  cascade invalidation via Workspace, mark dependents dirty
feature request       -->  ensure_compiled() --> compile if dirty --> serve query
```

## Compilation Pipeline

When `ensure_compiled()` determines a rebuild is needed, it runs the following pipeline:

1. **`fill_compile_args()`** -- Resolve compile flags from the CDB. For header files without a CDB entry, falls back to header context resolution: finds a host source that transitively includes the header, synthesizes a preamble containing all code above the `#include` line, and injects it via `-include`.

2. **`ensure_deps()`** -- Prepare all compilation dependencies:
   - **PCMs**: If C++20 modules are used, `CompileGraph::compile_deps()` topologically walks the module DAG and builds any missing PCM files via stateless workers.
   - **Buffer-scanned imports**: The in-memory buffer is scanned for `import` directives that may not yet be in the disk-based CompileGraph (e.g. the user just typed `import std;` without saving).
   - **PCH**: Calls `ensure_pch()` (see below).
   - **PCM paths**: `Workspace::fill_pcm_deps()` collects all available PCM paths as `-fmodule-file` arguments.

3. **Send to stateful worker** -- `CompileParams` (path, text, flags, PCH path, PCM paths) is sent to the worker via `WorkerPool::send_stateful()` with path-affinity routing.

4. **Process result** -- On success: clear `ast_dirty`, store `OpenFileIndex` from the returned TUIndex, record dependency snapshot, publish diagnostics. On failure or generation mismatch: leave `ast_dirty` true.

## PCH Content-Addressing

The preamble is the block of preprocessor directives at the top of a file. clice computes `compute_preamble_bound()` to find where the preamble ends, then hashes the preamble text with xxh3_64bits to produce a deterministic filename:

```
.clice/cache/pch/{hash:016x}.pch
```

PCH entries are stored in `Workspace.pch_cache`, keyed by file `path_id`. Each `PCHState` contains:

- `path`: the .pch file on disk.
- `bound`: preamble byte boundary.
- `hash`: xxh3 hash of the preamble text.
- `deps`: `DepsSnapshot` for staleness checking.
- `building`: shared event for concurrent build deduplication.

Session stores a lightweight `PCHRef` (path_id, hash, bound) that references the Workspace entry. If another coroutine is already building a PCH for the same file, subsequent callers wait on the `building` event rather than starting a duplicate build.

If the preamble is incomplete (user is still typing an `#include` directive), the PCH rebuild is deferred and the old PCH is reused if available.

## Two-Layer Staleness Detection

clice uses a two-layer strategy to avoid unnecessary recompilation when dependency files are touched but not actually modified.

### Layer 1: Fast mtime check

`DepsSnapshot` records a `build_at` timestamp at compile time. On staleness check, each dependency file is `stat()`-ed. If all mtimes are <= `build_at`, the artifact is fresh. This requires zero I/O beyond `stat()`.

### Layer 2: Content hash fallback

For files whose mtime has changed since `build_at`, their content is re-hashed with xxh3_64bits and compared against the stored hash in `DepsSnapshot.hashes`. If the hash matches, the file was "touched" (e.g. by a build system) but not actually modified -- the rebuild is skipped.

`DepsSnapshot` stores parallel arrays:

- `path_ids`: interned path IDs for each dependency file.
- `hashes`: xxh3 content hashes captured after the last successful compilation.
- `build_at`: timestamp of the compilation.

Staleness is checked in two places:

- `Compiler::is_stale()`: checks both AST deps and PCH deps for the Session.
- `deps_changed()`: the shared utility used for PCH, PCM, and AST staleness.

## Persistent Cache

PCH and PCM cache metadata is serialized to `cache.json` in the `.clice/` directory. This allows cached artifacts to survive server restarts.

- `Workspace::load_cache()` restores entries at startup (only if the referenced `.pch`/`.pcm` files still exist on disk).
- `Workspace::save_cache()` is called after every successful PCH or PCM build.
- `Workspace::cleanup_cache()` removes stale files older than 7 days (configurable via `max_age_days` parameter, default 7). Cleanup runs at startup before loading.

## Generation Counter (ABA Prevention)

The Session's `generation` counter prevents stale compilation results from being applied after concurrent edits.

1. `didChange` increments `generation` and sets `ast_dirty = true`.
2. `ensure_compiled()` captures `generation` before sending work to the worker.
3. After the worker returns, the captured generation is compared with the current value.
4. If they differ (the user edited the file while compilation was in flight), the result is **discarded**: `ast_dirty` stays true, and the next feature request will trigger a fresh compilation cycle.

This prevents the ABA problem where two rapid edits could cause an intermediate compilation result to overwrite state that should reflect the latest edit.

## Concurrency Model

Multiple concurrent feature requests for the same file each call `ensure_compiled()`. The first one launches a **detached compile task** via `loop.schedule()`. Subsequent callers wait on the shared `Session::PendingCompile` event.

The detached task is immune to LSP `$/cancelRequest` -- cancellation only affects the waiting feature request, not the compilation itself. This prevents a race where cancellation would wake all waiters, each starting a new compile.
