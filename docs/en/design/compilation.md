# Compilation

## Pull-Based Compilation Model

clice uses a **pull-based** compilation model. Document events (`didOpen`, `didChange`) only update Session state -- they never trigger compilation directly. Instead, compilation is triggered lazily when a feature request (hover, semantic tokens, go-to-definition, etc.) needs an up-to-date AST.

This design avoids wasted work: if the user types rapidly, only the final state is compiled when a feature request arrives.

```
didOpen  / didChange  -->  update buffer text, increment generation, mark AST dirty
didSave               -->  cascade invalidation via Workspace, mark dependents dirty
feature request       -->  ensure compiled --> compile if dirty --> serve query
```

## Compilation Pipeline

When a rebuild is needed, the following pipeline runs:

1. **Resolve compile flags** from the CDB. For header files without a CDB entry, falls back to header context resolution: finds a host source that transitively includes the header, synthesizes a preamble, and injects it via `-include`.

2. **Prepare dependencies**:
   - **PCMs**: If C++20 modules are used, the module DAG is topologically walked and missing PCM files are built via stateless workers.
   - **Buffer-scanned imports**: The in-memory buffer is scanned for `import` directives that may not yet be in the disk-based dependency graph (e.g. the user just typed `import std;` without saving).
   - **PCH**: Built or reused (see below).
   - **PCM paths**: All available PCM paths are collected as `-fmodule-file` arguments.

3. **Send to stateful worker** with path-affinity routing. The worker builds the AST, returns diagnostics and index data.

4. **Process result**: On success, clear dirty state, store the per-file index, record a dependency snapshot, and publish diagnostics. On failure or generation mismatch, leave the AST dirty for the next request.

## PCH Content-Addressing

The preamble is the block of preprocessor directives at the top of a file. clice finds where the preamble ends, hashes the preamble text with xxh3, and uses the hash as a deterministic filename:

```
.clice/cache/pch/{hash:016x}.pch
```

Files with identical preambles share the same PCH. The Session stores a lightweight reference (path, hash, bound) to the Workspace-level PCH entry. If another coroutine is already building a PCH for the same file, subsequent callers wait on a shared event rather than starting a duplicate build.

If the preamble is incomplete (user is still typing an `#include` directive), the PCH rebuild is deferred and the old PCH is reused if available.

## Two-Layer Staleness Detection

clice uses a two-layer strategy to avoid unnecessary recompilation when dependency files are touched but not actually modified.

### Layer 1: Fast mtime check

After each successful compilation, a dependency snapshot records a `build_at` timestamp. On staleness check, each dependency file is `stat()`-ed. If all mtimes are <= `build_at`, the artifact is fresh. This requires zero I/O beyond `stat()`.

### Layer 2: Content hash fallback

For files whose mtime has changed since `build_at`, their content is re-hashed with xxh3 and compared against the stored hash. If the hash matches, the file was "touched" (e.g. by a build system) but not actually modified -- the rebuild is skipped.

The dependency snapshot stores interned path IDs, content hashes, and the build timestamp. This is used for AST, PCH, and PCM staleness checks.

## Persistent Cache

PCH and PCM cache metadata is serialized to `cache.json` in the `.clice/` directory. This allows cached artifacts to survive server restarts.

- On startup, entries are restored (only if the referenced `.pch`/`.pcm` files still exist on disk).
- After every successful PCH or PCM build, the cache is saved.
- Stale files older than 7 days are cleaned up at startup before loading.

## Generation Counter (ABA Prevention)

The Session's generation counter prevents stale compilation results from being applied after concurrent edits.

1. `didChange` increments the generation and marks the AST dirty.
2. Before sending work to the worker, the current generation is captured.
3. After the worker returns, the captured generation is compared with the current value.
4. If they differ (the user edited the file while compilation was in flight), the result is **discarded**: the AST stays dirty, and the next feature request will trigger a fresh compilation.

## Concurrency Model

Multiple concurrent feature requests for the same file all need a compiled AST. The first one launches a **detached compile task**. Subsequent callers wait on a shared event.

The detached task is immune to LSP `$/cancelRequest` -- cancellation only affects the waiting feature request, not the compilation itself. This prevents a race where cancellation would wake all waiters, each starting a new compile.
