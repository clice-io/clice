# Incremental Compilation

## Background

C++ `#include` is textual substitution -- the preprocessor inserts the contents of included header files verbatim into the source file. A source file with only a few dozen lines of user code can expand to tens of thousands of lines or more after `#include` expansion. For example, a single `#include <vector>` directive pulls in thousands of lines of standard library code; add in project headers and the preprocessed output can easily exceed a hundred thousand lines.

A language server must recompile files after every user edit to provide up-to-date diagnostics, completions, and semantic information. Fully compiling those hundred thousand lines every time would take several seconds -- clearly unacceptable.

However, a key characteristic emerges when observing actual editing behavior: the `#include` region at the top of a file rarely changes, while the user code at the bottom changes constantly. In a typical editing session, the user might modify code hundreds of times without touching the `#include` region once.

```cpp
#include <vector>
#include <string>
#include <map>
#include "project/config.h"
#include "project/logging.h"
// ── preamble ↑ changes slowly, compilation result can be cached ──
// ── user code ↓ changes rapidly, must be recompiled each time    ──

void process(const std::vector<std::string>& data) {
    // ...
}
```

Based on this observation, C++ language servers commonly adopt a **preamble separation** strategy: split the file into the preamble (the preprocessor directive region at the top) and the remaining user code. The preamble is compiled into a precompiled header (PCH) and cached; subsequent compilations load the PCH directly and only reprocess the user code. This reduces post-edit recompilation to just a few dozen to a few hundred lines, typically bringing latency under one second.

clangd also adopts this strategy, but has several design-level shortcomings in invalidation detection and lifecycle management:

- **High memory usage**. clangd keeps preamble compilation artifacts in process memory. In large projects, preamble ASTs from multiple open files consume substantial memory, and usage grows over long-running sessions (clangd [#251](https://github.com/clangd/clangd/issues/251), [#115](https://github.com/clangd/clangd/issues/115)).

- **PCH file leaks on crash**. clangd stores on-disk PCH in temporary files. On crash, RAII cleanup cannot execute and temp files persist in `/tmp`. On multi-user servers, accumulated leaked files can exhaust `/tmp` space (clangd [#209](https://github.com/clangd/clangd/issues/209), [#255](https://github.com/clangd/clangd/issues/255)).

- **Imprecise invalidation detection**. When a header file is touched without changing its content (common during build tool dependency scanning, VCS branch switching), relying solely on modification time (mtime) triggers unnecessary PCH rebuilds. In large projects, these false positives noticeably degrade the editing experience.

- **Cold start on restart**. PCH is not persisted across sessions. After a server restart, PCH must be rebuilt for all open files.

clice redesigns the incremental compilation mechanism to address these issues: disk-persisted content-addressed PCH storage, two-layer invalidation detection, a pull-based compilation model, and preamble completeness checking.

## Design

Incremental compilation is organized around four core concepts: preamble separation defines "what to cache," two-layer invalidation detection defines "when to rebuild," pull-based compilation defines "when to trigger," and content-addressed storage defines "how to store."

### Preamble Separation

The preamble is the region at the top of a source file consisting of preprocessor directives (`#include`, `#define`, `#pragma`, etc.) and module declarations (`module;`). Its end position is identified by a byte offset (bound) -- the byte position just before the first line of non-preprocessor content.

The preamble is compiled into a PCH file and cached on disk. Subsequent compilations load the PCH and only need to process user code after the bound. If the preamble is empty (bound is zero), no PCH is needed and the entire PCH flow is skipped.

`PCHState` is a PCH cache entry, containing:

- The PCH file path on disk
- A hash of the preamble content
- The preamble byte boundary (bound)
- A dependency snapshot (`DepsSnapshot`, see below)
- DocumentLink information extracted from the PCH (`#include` directive positions and targets, for the editor to display clickable links)

### Two-Layer Invalidation Detection

A PCH caches the preprocessing results of all headers included in the preamble. When the content of any dependency header changes, the PCH is stale and needs rebuilding. The challenge is precisely determining "whether the content actually changed."

The most direct approach is checking file modification times (mtime): if all dependency files have mtimes no later than the PCH's build timestamp, no file has been modified. This check requires only `stat` system calls and is very fast. However, mtime checks produce false positives: build tool dependency scanning, VCS branch switching, editor auto-save, and similar operations update mtime without changing file content.

An alternative is directly comparing content hashes: recompute the hash of every dependency file on each check and compare against the hashes recorded at build time. This approach is perfectly precise but requires reading and hashing the contents of all dependency files. A typical C++ file may depend on hundreds of headers, making the I/O cost of full hashing on every check non-negligible.

clice combines both into a two-layer detection strategy:

- **Layer 1 (mtime fast screening)**: Iterate over all dependency files and compare each file's mtime against the PCH's build timestamp. If all mtimes are no later than the build timestamp, the PCH is valid and can be reused directly.
- **Layer 2 (content hash precise verification)**: For files flagged as "possibly modified" by Layer 1 (mtime later than the build timestamp), recompute their xxh3 content hash and compare against the hash recorded at build time. Only trigger a rebuild when hashes differ.

Layer 1 filters out the vast majority of unchanged files (the common-case path). Layer 2 eliminates mtime false positives (build tool touches, VCS checkouts, etc.). The combined effect: PCH is rebuilt only when the content of a dependency file has actually changed.

`DepsSnapshot` is the underlying data structure for two-layer detection, captured when a PCH build completes. It records the path identifiers, content hashes, and build timestamp of all dependency files.

### Pull-Based Compilation

clice uses a pull-based compilation model: compilation is not triggered immediately on file change but on demand when a feature request (hover, completion, semantic highlighting, etc.) requires an up-to-date AST.

When the user edits a file (`didChange`), the master process only updates the in-memory file content and marks the AST as dirty (`ast_dirty`), without starting any compilation. When a feature request arrives, the compilation service checks whether the AST is dirty or has become stale due to external file changes. If recompilation is needed, it first ensures the PCH and module dependencies are ready, then dispatches the compilation task to a worker process.

The benefit of this model is avoiding wasteful compilations during rapid successive keystrokes. The user may trigger a dozen `didChange` events per second, but compilation only executes when an actual result is needed -- such as a hover or completion request.

> Note that "external file changes" and "user edits" are two independent dirty-marking paths. User edits mark `ast_dirty` via `didChange`; external file changes (e.g., a dependency header modified on disk) are discovered dynamically via two-layer invalidation detection before compilation.

### Content-Addressed PCH Storage

PCH files on disk are named by the hash of the preamble content (e.g., `a3f7e8c1d2b4f6e9.pch`), implementing content-addressed storage. This provides two benefits:

- **Disk sharing**: Different files with identical preamble content naturally share the same PCH file on disk, with no additional deduplication logic needed.
- **Cross-session persistence**: PCH cache metadata (path, hash, boundary, dependency snapshot) is serialized to a `cache.json` file on disk. On server restart, this metadata is loaded and each PCH's validity is verified through two-layer invalidation detection, avoiding the need to rebuild all PCHs on a cold start.

When preamble content changes, the new PCH uses a different hash for its filename and the old file becomes orphaned. A cleanup mechanism periodically reclaims orphaned PCH files that have not been used beyond a certain age.

## Implementation

### Preamble Boundary Computation

The preamble boundary is determined through lexical scanning: the project's `Lexer` scans line by line from the start of the file, identifying preprocessor directives beginning with `#` and `module;` global module fragment declarations. Scanning stops at the first line of non-preprocessor content, and the byte offset at that point is returned as the boundary.

This lexer-based detection does not require starting a full preprocessor and is very fast.

### Preamble Completeness Check

Before triggering a PCH rebuild, the preamble must be checked for syntactic completeness. Two typical incomplete states:

```cpp
#include "lib          // unclosed quote, user is typing a filename
import std.core        // missing semicolon, user is typing a module declaration
```

Building a PCH from an incomplete preamble produces a PCH containing erroneous preprocessor state. Subsequent compilations loading this erroneous PCH would see a flood of spurious errors. Therefore, when the preamble is detected as incomplete, the rebuild is deferred and the old PCH (if one exists) continues to be used.

### PCH Build Pipeline

When a compilation request requires a PCH, the following pipeline executes:

```
Compute preamble boundary and hash
         │
         ▼
   ┌─ Cache hit? ──── yes → Reuse cached PCH
   │     │
   │    no
   │     │
   │     ▼
   │  Preamble complete? ── no → Defer rebuild, keep old PCH
   │     │
   │   yes
   │     │
   │     ▼
   │  Another coroutine building? ── yes → Wait for completion, use result
   │     │
   │    no
   │     │
   │     ▼
   │  Dispatch to stateless worker to build PCH
   │     │
   │     ▼
   └─ Update cache, capture dependency snapshot
```

Cache hit requires two conditions: the preamble hash matches the cached value (preamble content unchanged), and two-layer invalidation detection passes (dependency file contents unchanged). Both conditions must be satisfied simultaneously.

PCH builds are executed by stateless worker processes (see [multi-process architecture](multi-process.md)). The worker uses Clang's Preamble compilation mode, processing only the preamble portion before the bound. Upon completion, it returns the PCH file path and list of dependency files.

### Concurrent Build Serialization

Multiple feature requests may simultaneously trigger a PCH build for the same file. `PCHState` contains a shared event (`building`): the first coroutine to initiate a build sets this event, and subsequent coroutines that find the event present wait for its completion and then use the build result. This ensures the PCH for a given file is built only once.

### Dependency Snapshot Timing Guarantee

The `DepsSnapshot` build timestamp (`build_at`) is obtained **before** computing file hashes. This ordering ensures there is no time window in which a modification could be missed:

If a file is modified during hash computation, its mtime will be later than `build_at`. On the next two-layer detection, Layer 1 will flag this file as "possibly modified," and Layer 2 will recompute its hash and discover the change.

If the order were reversed -- hashing first, then obtaining the timestamp -- a window could arise: a file modified between hash computation and timestamp acquisition would have an mtime no later than `build_at`, causing the modification to be missed.

### Overall Compilation Flow

When a feature request arrives, the compilation pipeline executes in this order:

1. Check whether the AST is cached and not stale -- if so, reuse it directly
2. If C++20 modules are used, ensure module dependencies are ready (see [module compilation](module-graph.md))
3. Ensure the PCH is ready
4. Dispatch the compilation task to a stateful worker process with the PCH path and module file paths
5. The worker loads the PCH and compiles only the user code after the preamble

AST dependency files (headers `#include`d after the preamble) are also tracked via `DepsSnapshot`, using the same two-layer invalidation detection. Even if the user has not edited the current file, if a dependency header is modified on disk, the next feature request will trigger recompilation.

### Interaction with Compilation Context

For non-self-contained headers, the compilation context system synthesizes a prefix file and injects it via `-include` into the compilation arguments (see [compilation context](compilation-context.md)). This injected prefix file is processed by Clang during the preamble compilation phase and is therefore naturally covered by PCH caching. The PCH build pipeline handles source files and headers uniformly.

### Cache Persistence

PCH and PCM cache metadata are persisted to disk via a `cache.json` file. This file is updated after each successful build and loaded on server startup. Writes use a write-to-temp-then-atomic-rename pattern to prevent file corruption from mid-write crashes.

After loading the cache on startup, all PCH entries are validated through two-layer invalidation detection. Stale entries are automatically rebuilt on the next compilation, requiring no special cache consistency recovery logic.

## FAQ

- **Why two-layer detection instead of content hashing alone?** Content hashing is precise but requires reading the contents of all dependency files. A typical C++ file may depend on hundreds of headers, making the I/O cost of full hashing on every check non-negligible. The mtime fast-screening layer reduces the number of files that need hashing to those "touched since the last build," which in the common case is typically zero.

- **Why full PCH rebuild each time? Can it be updated incrementally?** Clang supports chained PCH: each `#include` in the preamble is built as an independent PCH link, with each link depending on the previous link's compilation artifact. When the user adds a new `#include` at the end of the preamble, only one new link needs to be appended to the existing chain rather than rebuilding the entire preamble. Benchmarks show (PR [#405](https://github.com/ykiko/clice/pull/405)) that for a preamble of 70 C++ standard library headers, incrementally appending one `#include` takes about 36ms compared to about 1230ms for a full rebuild (35x speedup). Chained PCH AST load latency is virtually unaffected (+2% to +6%). clice plans to adopt chained PCH to optimize incremental rebuild performance; this is currently in the experimental stage.

- **Why pull-based compilation instead of push-based?** The key reason is that clice persists all files' PCH to disk, resulting in far more cached files than clangd's in-memory model (clangd keeps only a few active files' preambles via LRU). When a header file is modified, a large number of files' PCHs may be affected. With push-based compilation, all affected PCHs would need to be rebuilt immediately on header modification -- an unacceptable volume. Pull-based compilation defers rebuilding until a feature request arrives, rebuilding only the PCH for the file the user currently needs. Since PCH loading itself is fast (see next item), the latency introduced by on-demand rebuilding is small.

- **Is disk PCH slower than in-memory PCH?** The practical impact is minimal. Clang loads PCH using mmap to map the file into memory, avoiding a full read-copy. More importantly, Clang deserializes AST nodes from the PCH lazily -- only nodes actually referenced during compilation are deserialized, and most PCH content is never accessed. Thus PCH loading performance depends primarily on how the binary file is mapped, with no fundamental difference between a disk file and an in-memory buffer. The benefits of disk PCH -- cross-restart persistence, no resident process memory usage, content-addressed sharing -- make this trade-off worthwhile.

## Known Limitations

- **Per-file independent caching**. The PCH cache is keyed by file path identifier. Even if two files have identical preamble content, they have independent cache entries -- each performing its own invalidation detection and build. While the PCH files on disk are shared through content-addressed naming, cache metadata (dependency snapshots, build state, etc.) is not shared. The improvement direction is to key the cache by preamble content hash plus compilation flags, enabling cross-file cache metadata sharing.

- **Full rebuild**. Any content change in a dependency file triggers a full PCH rebuild, with no way to rebuild only the affected portion. The improvement direction is to adopt chained PCH (see FAQ), limiting the rebuild scope to the chain links after the point of change.

- **Compilation flags not part of cache key**. PCH disk filenames and cache lookups do not consider compilation flags. When two files have the same preamble text but different compilation flags (e.g., `-D`), they may incorrectly share a PCH. The improvement direction is to include preprocessing-relevant compilation flags in the cache key.

- **Incomplete preamble completeness check**. The current completeness check only covers unclosed quotes and missing semicolons in `#include`/`import` directives. Other types of incomplete edits (e.g., typing a `#define` value) are not detected. The impact of building a PCH from such an incomplete preamble on subsequent compilation has not been thoroughly tested. Further investigation is needed into Clang's behavior when processing incomplete preprocessor directives, to determine whether the completeness check scope should be extended.

- **No proactive diagnostics push after header save**. Under the current pure pull-based model, after a user saves a header file, open source files that depend on it do not immediately update diagnostics -- the user must trigger an action on the source file (e.g., hover, edit) for the two-layer invalidation detection to discover the change and trigger recompilation. The improvement direction is to check affected open sessions on `didSave` and proactively trigger compilation for them (a hybrid push/pull model).
