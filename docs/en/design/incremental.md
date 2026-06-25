# Incremental Compilation

## Background

A language server must recompile files every time the user edits code in order to provide up-to-date diagnostics, completions, and semantic information. Full compilation of a C++ file can take several seconds -- a typical source file may expand to tens or even hundreds of thousands of lines after `#include` expansion. Recompiling from scratch on every keystroke would produce an unacceptable user experience.

clice achieves incremental compilation through **preamble separation**: each source file is split into two parts -- the leading `#include` region (the preamble) and the remaining user code. The preamble is compiled into a precompiled header (PCH) and cached on disk; subsequent compilations load the PCH directly and only reprocess the user code. For example, a file containing `#include <iostream>` expands to roughly 20,000 lines; once the PCH is built, recompilation covers only the few dozen lines the user actually wrote.

The core challenges of this mechanism lie in PCH **invalidation detection** and **lifecycle management**:

**Precision of invalidation detection**: A PCH caches the preprocessing results of every `#include`d header. When any header is modified, the PCH is out of date. However, naively checking file modification times (mtime) causes many false positives -- build tools frequently touch files without changing their contents (e.g., `cmake --build` dependency scanning, VCS branch switches), triggering unnecessary PCH rebuilds.

**Controlling rebuild overhead**: Building a PCH requires full preprocessing and serialization, typically taking hundreds of milliseconds to several seconds. If every save triggers a PCH rebuild, users will notice perceptible lag. The system must accurately determine whether the PCH is truly stale and only rebuild when necessary.

**Concurrency coordination**: Multiple files may share the same preamble. When a PCH needs to be rebuilt, several compilation requests may attempt to trigger the rebuild simultaneously. The system must ensure only one build executes while other requests wait for the result.

**Incomplete edits**: While the user is typing `#include "`, the preamble is in an incomplete state. Triggering a PCH rebuild at this point would produce an erroneous PCH, causing a flood of spurious errors in subsequent compilations.

In clangd, incremental compilation issues have long plagued users: diagnostics not updating after header modifications (requiring the user to reopen the file or restart the server), overly frequent PCH rebuilds causing editing lag in large projects, and occasional PCH corruption during simultaneous multi-file editing that prevents the entire project from compiling correctly.

## Design

### Core Idea

The fundamental strategy of incremental compilation is to split a source file into the **slowly changing part** (preamble) and the **rapidly changing part** (user code), handling each separately. The preamble is compiled into a PCH cached on disk and only rebuilt when the content of a dependent header actually changes; the user code is recompiled by loading the PCH after each edit, which is very cheap.

The core challenge of PCH management is **precise invalidation detection** -- finding the right balance between "rebuilding too aggressively" and "using a stale cache." clice achieves precise detection through two layers of invalidation checking (mtime fast screening + content hash verification), combined with preamble completeness checking and serialized concurrent builds, ensuring the PCH is always in a correct and efficient state.

### Preamble Boundary Detection

At the heart of PCH is the preamble -- the region at the top of a source file consisting of preprocessor directives. The language server must precisely determine where the preamble ends (as a byte offset) so that only this portion is compiled into the PCH.

Preamble boundary detection is based on lexical scanning: scanning line by line from the start of the file, identifying preprocessor directives beginning with `#` (`#include`, `#define`, `#pragma`, etc.) and global module fragment declarations (`module;`). Scanning stops at the first line of non-preprocessor content. The returned byte offset marks the preamble boundary.

If the boundary is zero (the file has no preprocessor directives), no PCH is needed -- the PCH build step is skipped entirely and any existing PCH cache entry for that file is cleared.

### Two-Layer Invalidation Detection

PCH invalidation detection must answer the question: "Since the last build, has the content of any header file included in the PCH changed?"

clice uses a two-layer detection strategy that balances precision and performance:

**Layer 1: Modification Time (mtime) Fast Screening**

The system iterates over all dependency files of the PCH and retrieves each file's mtime. If every file's mtime is no later than the PCH's build timestamp (build_at), no file has been modified and the PCH is still valid -- it is reused directly. This layer requires only stat system calls, making it very cheap.

**Layer 2: Content Hash Precise Verification**

For files whose mtime is later than build_at (flagged as "possibly modified" by the first layer), the system recomputes their content's xxh3 hash and compares it against the hash recorded at build time. If the hashes match, the file content has not actually changed (it was merely touched), and the PCH is still valid. A rebuild is triggered only when the hashes differ.

The core value of this two-layer strategy is **eliminating false positives**: build tool touch operations, VCS checkout operations, and similar actions update mtime without changing content. Relying solely on mtime would cause many unnecessary PCH rebuilds. The second layer's content hashing filters out these false positives with modest additional overhead (hashing only the "suspect" files).

### DepsSnapshot: Dependency Snapshot

The foundation of two-layer detection is DepsSnapshot -- a snapshot of dependency state captured when a PCH build completes. It records:

- **path_ids**: Path identifiers for all dependency files
- **hashes**: The content hash (xxh3_64bits) of each dependency file at build time
- **build_at**: The timestamp when the snapshot was captured

**Timing correctness**: build_at is obtained **before** computing file hashes. This ensures that if a file is modified during hash computation (its mtime becomes later than build_at), the next detection cycle will flag it as "possibly modified" in the first layer and pass it to the second layer for verification. There is no time window (TOCTOU) in which a modification could be missed.

### PCH Build Pipeline

When a compilation request requires a PCH, the following pipeline is triggered via ensure_pch():

**1. Compute preamble boundary and hash**

The preamble portion of the file content is extracted and its xxh3 hash is computed. This hash is used for PCH file naming on disk and also serves as a cache validity check.

**2. Look up the cache**

The system checks whether a cache entry already exists for the file in the Workspace's pch_cache. If an entry exists and:
- The preamble hash matches (preamble content has not changed)
- Two-layer invalidation detection passes (dependency file contents have not changed)

The cached PCH is reused directly with no rebuild needed.

**3. Check preamble completeness**

If the cache misses or is invalidated, the system checks whether the preamble is complete before triggering a rebuild -- specifically, whether there are unclosed quotes (e.g., `#include "lib`) or unterminated module declarations (e.g., `import foo` missing a semicolon). If incomplete, the user is likely editing the preamble region; the rebuild is deferred and the old PCH (if one exists) continues to be used.

**4. Serialize concurrent builds**

The system checks whether another coroutine is already building a PCH for this file. If so, it waits for the completion event and then uses the build result. This is implemented through a shared event (kota::event) in PCHState -- the first coroutine to initiate the build sets the event, and subsequent coroutines wait on the same event.

**5. Dispatch to a stateless worker process**

The PCH build is dispatched as a high-priority task to a stateless worker process. The worker uses Clang's Preamble compilation mode (CompilationKind::Preamble), passing the full file content and preamble boundary to Clang via file remapping; Clang processes only the preamble portion before the boundary. Upon completion, the worker returns the PCH file path and the list of dependency files.

**6. Update the cache**

The build result is stored in pch_cache: the PCH file path, preamble hash, preamble boundary, and dependency snapshot. DocumentLink information from the PCH (positions and targets of `#include` directives) is also captured, enabling the editor to display clickable header links.

### PCH File Naming on Disk

PCH files on disk are named by the hash of the preamble content, implementing content-addressable storage. This allows different files with identical preamble content to **share the same PCH file on disk**. When preamble content changes, the new PCH file uses a different hash for its name, and the old file becomes orphaned. A cleanup mechanism periodically reclaims orphaned PCH files that have not been used beyond a certain age.

### PCH Cache Storage Model

The PCH cache (pch_cache) is currently keyed by the source file's path_id. This means that even if two files have identical preambles, they have independent cache entries -- each performing its own invalidation detection and rebuild. While the PCH files on disk are shared through content-addressable naming, the cache metadata (dependency snapshots, build timestamps, etc.) is not shared.

PCH cache metadata (path, hash, boundary, dependency snapshot) is persisted to a cache.json file on disk. This allows recovery on server restart, avoiding the need to rebuild all PCHs on a cold start.

### Compilation Triggering and Overall Flow

clice uses a **pull-based** compilation model -- compilation is not triggered immediately on file change but is triggered on demand when a feature request (hover, completion, semantic highlighting, etc.) requires an up-to-date AST. This avoids wasteful compilations during rapid successive keystrokes.

When a feature request requires an up-to-date AST, the compilation pipeline proceeds through these steps:

1. Check whether the AST is cached and not dirty -- if so, reuse it directly
2. Check whether the AST's dependencies are stale (using the same two-layer invalidation detection)
3. If recompilation is needed, first ensure C++20 module dependencies are ready (via CompileGraph; see the module compilation documentation)
4. Then ensure the PCH is ready (via ensure_pch)
5. Finally, dispatch the compilation task to a stateful worker process with the PCH path

The stateful worker loads the PCH and only needs to compile the user code after the preamble -- this is the core benefit of incremental compilation, reducing compilation time from several seconds to typically under one second.

### PCH and Header Compilation Context

For non-self-contained headers, the compilation context system synthesizes a prefix file (see the compilation context documentation) and injects it via `-include` to simulate the header's inclusion environment within its host source file. This injected prefix file appears in the header's compilation preamble and is therefore covered by PCH caching.

ensure_pch() handles headers and source files uniformly: whether the preamble contains the file's own `#include` region or injected prefix content, the same hashing, caching, invalidation detection, and build pipeline applies.

## Design Decisions and Trade-offs

**Why two-layer detection instead of content hashing alone?** While content hashing is precise, it requires reading and hashing the entire contents of every dependency file. A typical C++ file may depend on hundreds of headers, making the I/O cost of hashing all of them non-trivial. The mtime fast-screening layer reduces the number of files that need hashing to those "touched since the last build," which is typically far smaller than the total dependency count.

**Why not incremental PCH patching?** Ideally, when only one header undergoes a small change, it should be possible to patch the PCH incrementally rather than fully rebuilding it. However, Clang's PCH format does not support incremental patching -- it is a monolithic serialized snapshot of AST and preprocessor state. Incremental patching would require deep modifications to Clang's serialization layer, and the complexity is disproportionate to the benefit.

**Why doesn't the PCH hash include compilation flags?** Currently, the PCH hash is based solely on the preamble text content and does not include compilation flags (`-D`, `-std`, etc.). This is a deliberate trade-off -- it allows files with the same preamble text but different compilation flags to share a PCH file on disk. In practice, the preprocessing result of the same preamble under different compilation flags is usually identical (differences mainly come from macro definitions, which are typically reflected in `#define` directives within the preamble text). However, there are edge cases where this is incorrect -- for example, when a command-line `-D` defines a macro that affects header behavior, different compilation flags should use different PCHs.

**Why is preamble completeness checking important?** Building a PCH from an incomplete preamble produces a PCH containing erroneous preprocessor state. Subsequent compilations using this erroneous PCH will exhibit a flood of spurious errors. Deferring the rebuild until the preamble is complete avoids this cascade of errors.

## Known Limitations

- **Per-file independent caching**: pch_cache is keyed by path_id and cannot share cache metadata (dependency snapshots, build state, etc.) across different files with identical preambles. A future improvement is to key the cache by preamble content hash plus compilation flags, enabling true cross-file cache sharing while also addressing the issue of compilation flags not participating in the cache key, along with introducing LRU eviction.
- **Full rebuild**: Any content change in a dependency file triggers a full PCH rebuild. There is no way to rebuild only the affected portion. This is an inherent limitation of the Clang PCH format.
