# Symbol Index

## Background

Many features of a language server need to work across files. A user triggers "go to definition" in one file, and the target may be in any other file in the project; "find references" needs to scan every file in the project that might mention the symbol; call hierarchy and type hierarchy involve chains of symbol relationships across multiple files. To support these features, the language server must maintain a project-wide symbol index that records which files each symbol appears in and the semantic relationships between them (definition, reference, call, inheritance, etc.).

C++ makes index construction and maintenance particularly difficult.

The first level of difficulty is the compilation-context problem with header files. C++'s `#include` is textual substitution — the contents of a header file are inserted verbatim into the source file that includes it at compile time. This means the same header file can produce entirely different symbols under different compilation contexts:

```cpp
// crypto.h
#ifdef USE_OPENSSL
    using TLSContext = OpenSSLContext;
#else
    using TLSContext = BoringSSLContext;
#endif
```

When `crypto.h` is included by a source file that defines `USE_OPENSSL`, `TLSContext` is `OpenSSLContext`; when included by another source file, it is `BoringSSLContext`. Conditional compilation is the most obvious example, but include order and template instantiation can also cause header files to produce different symbol relationships under different contexts. If the index records only one context's results, users will see incorrect jump targets or incomplete reference lists after switching contexts.

The second level is scale. A mid-sized C++ project (a few thousand source files) produces hundreds of thousands of symbols after compilation; large projects (LLVM, Chromium) reach millions. The index system must maintain reasonable memory usage, build time, and query latency at this scale.

clangd's handling of both levels is insufficient. On the compilation-context front, clangd's background index stores only the last compilation result for each header — whichever source file is compiled last overwrites the previous index data. If a symbol reference exists only under a particular compilation context that is not the last one indexed, that reference is lost.

On the cross-file lookup front, clangd's background index stores symbol information in the index shard corresponding to the file where the symbol is declared. This declaration-file-centric storage model prevents reference counts from accumulating correctly across files (clangd [#23](https://github.com/clangd/clangd/issues/23)). Users also frequently encounter incomplete "find references" results — certain references only appear after manually opening the relevant files, at which point the dynamic index fills in the missing data (clangd [#516](https://github.com/clangd/clangd/issues/516), [#802](https://github.com/clangd/clangd/issues/802)). Additionally, when compilation commands change, clangd's staleness detection does not trigger re-indexing (clangd [#199](https://github.com/clangd/clangd/issues/199)), leaving the index data out of sync with the actual compilation state for extended periods.

clice's index system is redesigned to address these problems: it adopts a three-level index structure separating the global symbol directory from per-file sharded relation data, uses content-addressed deduplication to merge indexes from different compilation contexts, leverages FlatBuffers for on-demand lazy loading to control memory usage, and provides a real-time overlay for open files to ensure query result freshness during editing.

## Design

### Symbol Identity

The index system needs a way to identify the same symbol across files and translation units. clice uses `SymbolHash` (a 64-bit integer) as the unique identifier for each symbol.

`SymbolHash` is generated from Clang's USR (Unified Symbol Resolution). USR is a canonical string representation of symbol identity that encodes the symbol's fully qualified name: namespace, class name, function signature, template parameters, etc. For example, `std::vector<int>::push_back` and `std::vector<double>::push_back` produce different USRs. `SymbolHash` is the hash of the USR string.

`SymbolHash` has two key properties. First, cross-file consistency: the same symbol always has the same `SymbolHash` regardless of which file it appears in. `std::string` seen in file A and `std::string` seen in file B have the same hash, allowing all definitions and references to be associated through it — this is the foundation of cross-file navigation. Second, compactness: a 64-bit integer is better suited as a hash table key and for serialized storage than a variable-length USR string.

### Symbol Occurrences and Relations

The index stores two kinds of core data: symbol occurrences (`Occurrence`) and symbol relations (`Relation`).

An `Occurrence` records a symbol's presence at a source location, containing only a source range and the target symbol's `SymbolHash`. It answers the question "what symbol is under the cursor."

A `Relation` records richer semantic information, consisting of three elements: the relation kind (`RelationKind`), a source location, and a target symbol. Relation kinds cover common inter-symbol semantics:

- Definition and declaration (Definition, Declaration)
- References (Reference, WeakReference)
- Inheritance (Base, Derived)
- Calls (Caller, Callee)
- Type relationships (Interface, Implementation, TypeDefinition)
- Construction and destruction (Constructor, Destructor)

The two are stored separately because their query patterns differ. `Occurrence` is indexed by position — given a byte offset, binary search quickly locates the symbol under the cursor. `Relation` is indexed by `SymbolHash` — given a symbol, look up all its definitions, references, and call relationships. These two queries have contradictory sorting requirements; separate storage allows both to execute efficiently.

### Three-Level Index Hierarchy

clice's index is organized into three levels, each with a different lifecycle and responsibility:

```
TUIndex        Raw artifact from a single compilation, discarded after merging
    ↓ merge
ProjectIndex   Global symbol directory (which files a symbol appears in), resident in memory
MergedIndex    Per-file sharded relation data (exact positions and relations), loaded on demand
    ↑ overlay
FileIndex      Real-time overlay for open files (from in-memory AST)
```

**TUIndex** is the raw index data produced by compiling a translation unit. `SemanticVisitor` traverses the AST, generating `Occurrence` and `Relation` records for each symbol, organized by file into a `TUIndex`. Since a compilation involves the main file and all included headers, `TUIndex` internally maintains a separate `FileIndex` for each file involved. `TUIndex` also contains a `SymbolTable` (mapping symbol hashes to names and kinds) and an `IncludeGraph` (include relationships from this compilation). `TUIndex` is transient data, discarded after being merged into the persistent indexes.

**ProjectIndex** is the global symbol directory. It aggregates symbol information from all indexed translation units, maintaining a global symbol table: `SymbolHash` → symbol name, symbol kind, reference file bitmap. The reference file bitmap records which files the symbol appears in, stored using Roaring Bitmap compression.

`ProjectIndex` does not store exact symbol positions (offsets, line numbers). Its role is that of a "directory" — it tells you which files a symbol exists in, then you look up the exact positions in the corresponding `MergedIndex` shard. This separation keeps `ProjectIndex` compact enough to reside in memory at all times.

**MergedIndex** is the per-file sharded index storage layer. Each file in the project corresponds to one `MergedIndex` shard, storing all symbol occurrence positions and relation information for that file. This is the largest part of the index system by volume and the layer that actually serves queries. It supports lazy loading from disk — shards that are never queried need not be loaded into memory.

`MergedIndex`'s core capability is merging and deduplicating index data from different compilation contexts of the same file, detailed in the Implementation section.

**FileIndex** (the open-file overlay) resides in each open file's `Session`, produced by the most recent in-memory compilation. It stores the same types of data as a `MergedIndex` shard (`Occurrence` and `Relation`), but is never written to global state — it is used only as an overlay during queries, overriding disk-indexed data with results from the current edit buffer's compilation.

### Symbol Table

`SymbolTable` maps `SymbolHash` to symbol metadata — name and kind (Class, Function, Variable, etc.). It appears in two places: the global symbol table in `ProjectIndex` and the local symbol table in each `Session`. When looking up a symbol's name, the `Session` is checked first (more current); if not found, `ProjectIndex` is consulted.

### IncludeGraph

`IncludeGraph` records the include relationships from a single compilation. It consists of two parts: a path list (all file paths involved in the compilation) and `IncludeLocation` records (each indicating a file was included at a certain line, along with the source of the inclusion).

`IncludeGraph` serves two purposes. During `TUIndex` merging, it provides the mapping from compilation-unit-internal file IDs to project-global path IDs. Within `MergedIndex`, it is stored as part of the compilation context, with the include chain used for staleness detection.

## Implementation

### Index Construction

`TUIndex` construction is performed by `SemanticVisitor`: given a compilation unit, it traverses the AST, generating `Occurrence` and `Relation` records for each named declaration and macro. After traversal, each file's data is deduplicated and sorted — `Occurrence` entries are sorted by position to support binary search, `Relation` entries are sorted by kind and position for efficient filtering.

During construction, the main file's (source file's) `FileIndex` is extracted separately. This allows different treatment during merging — the main file is merged as a source-file context, while other files are merged as header contexts.

### Index Merging

`TUIndex` merging into the persistent indexes proceeds in two steps.

Step one: symbol information is merged into `ProjectIndex`. All symbols from the `TUIndex` are inserted into the global symbol table, and each symbol's reference file bitmap is updated — file IDs involved in this compilation are added to the bitmap. Path mapping is also completed at this step: path IDs internal to the `TUIndex` are converted to `ProjectIndex`'s global path IDs.

Step two: each file's `FileIndex` is merged into the corresponding `MergedIndex` shard. For the main file, compilation context information (build timestamp, include chain) is attached; for header files, header context information (include location identifier) is attached.

### Compilation-Context Deduplication

The core problem for `MergedIndex` is: the same header file is included by N source files, producing N `FileIndex` entries. If each were stored in full, storage would grow linearly with the number of translation units. But in practice, the vast majority of headers produce identical index data under different compilation contexts — the same symbols appear at the same positions, producing the same relations. Only headers like the `crypto.h` example above, affected by conditional compilation, produce different index content under different contexts.

`MergedIndex` solves this through content-addressed deduplication. Each `FileIndex` has its SHA-256 content hash computed before merging. `FileIndex` entries with the same hash have identical content and share the same canonical ID (an auto-incrementing integer identifier).

Specifically, `Occurrence` and `Relation` entries inside `MergedIndex` are not simple lists — each entry is associated with a Roaring Bitmap recording which canonical IDs it belongs to. When a new `FileIndex` is merged in:

1. Compute its SHA-256 hash
2. Check the cache: if this hash already exists, the data is identical — reuse the existing canonical ID and increment its reference count
3. If it is a new hash, allocate a new canonical ID, insert all `Occurrence` and `Relation` entries, and associate them with this new ID

When a compilation context is removed (e.g., a source file is deleted from the project), the corresponding canonical ID's reference count is decremented. Canonical IDs whose reference count reaches zero are marked into the "removed" set. During queries, data belonging to the removed set is filtered out.

This design means storage depends on the number of distinct index contents rather than the number of compilation contexts. For most headers, regardless of how many source files include them, only one copy of the data is stored.

### Compilation Context Types

`MergedIndex` internally distinguishes two types of compilation contexts:

- **CompilationContext**: Produced when a file is compiled directly as a source file. Records the build timestamp and include chain (for staleness detection), along with the corresponding canonical ID. A file can have multiple `CompilationContext` entries, corresponding to different compilation commands in the compilation database.
- **HeaderContext**: Produced when a file is included as a header by another source file. Records the including source file and include location, along with the corresponding canonical ID.

These two context types work in concert with the [compilation context](compilation-context.md) system. During queries, context types need not be distinguished — all context data has already been unified through canonical ID Bitmaps. During staleness detection, the `CompilationContext`'s include chain is used to determine whether re-indexing is needed.

### Lazy Loading

`MergedIndex` is serialized using FlatBuffers. FlatBuffers' design allows queries to be executed directly on serialized data without deserializing into in-memory structures. `MergedIndex` leverages this to implement a two-tier access model:

- **Read-only path**: After loading from disk, `MergedIndex` remains as the raw memory-mapped buffer. Query operations execute directly on the FlatBuffers data, with zero deserialization overhead.
- **Read-write path**: When modifications are needed (merging new data or removing old contexts), the FlatBuffers data is first deserialized into in-memory structures, and subsequent operations are performed on those structures. Modified shards are re-serialized when saved.

At startup, only `ProjectIndex` (relatively compact) needs to be loaded. `MergedIndex` shards are loaded on demand, and most shards are never accessed in a single session.

### Query Flow

Using "find references" as an example to illustrate the full cross-file query flow:

1. In the current file, use the cursor's byte offset to binary-search the `Occurrence` list and obtain the `SymbolHash` of the symbol under the cursor
2. Look up the `SymbolHash`'s reference file bitmap in `ProjectIndex` to get all files containing the symbol
3. Query each file in the list individually:
   - If the file is currently open (has an active `Session`), use the `Session`'s `FileIndex`, skipping the corresponding `MergedIndex` shard
   - If the file is not open, load the corresponding `MergedIndex` shard and look up relations in it
4. Aggregate all `Relation` entries found across files (filtering by the target `RelationKind`), convert to LSP positions, and return to the client

In step 3, open files preferentially use the `Session`'s `FileIndex` rather than `MergedIndex`, because the buffer content may differ from disk. The `Session`'s `FileIndex` comes from in-memory compilation results that more accurately reflect the code the user is currently seeing. Only when the `Session`'s AST is in a dirty state (the user has edited but the file has not been recompiled) does the system fall back to `MergedIndex`.

> Converting offsets to LSP positions requires the file content and a line-start offset table. `MergedIndex` shards also store the corresponding file's content and line-start table, so this conversion can be performed even for files that are not open.

### Staleness Detection

Staleness detection determines whether a file needs to be re-indexed. The `MergedIndex` shard stores the build timestamp and include chain. During detection, the last modification time (mtime) of each file in the include chain is checked. If any file's mtime is later than the build timestamp, the dependency has been updated and re-indexing is needed.

This detection is conservative — an mtime change does not necessarily mean the content changed (e.g., a `touch` operation, branch switching). But a false positive only results in one extra indexing pass, never a missed update.

### Background Indexing Scheduling

Background indexing scheduling must balance index timeliness against interference with user interaction. The index module employs the following strategies:

- **Queue with idle delay**: Files that need indexing are added to a queue, and processing begins only after the editor has been idle for a configurable period. This avoids triggering index tasks during rapid editing.
- **Concurrency control with memory monitoring**: The number of concurrent index tasks has a configurable upper limit. During indexing, system memory usage is dynamically monitored — concurrency is automatically reduced under memory pressure and gradually restored when memory recovers.
- **Priority management**: User-initiated operations (such as compiling an open file) pause background indexing. Indexing resumes after the operation completes, ensuring user request latency is not affected by background indexing.
- **Result merging and persistence**: Each index task compiles a file and builds a `TUIndex` in a stateless subprocess. The result is serialized and sent back to the main process, which merges it into `ProjectIndex` and `MergedIndex`. After indexing completes, modified shards are written back to disk so they can be loaded directly on the next startup.

## FAQ

- **Why separate `ProjectIndex` and `MergedIndex` instead of using a single unified index?**

  If position information were also stored in `ProjectIndex`, its size would balloon dramatically, making it impossible to keep in memory. Without `ProjectIndex`, every cross-file query would need to traverse all `MergedIndex` shards to locate files containing the symbol — in a project with tens of thousands of files, loading that many shards is unacceptable. `ProjectIndex` serves as a lightweight directory layer that first narrows the search scope to a handful of specific files, then precise lookup happens in the corresponding shards.

- **Why don't open-file indexes write to global state?**

  Buffer content being edited by the user may be incomplete code with syntax errors. If this temporary state were written to the global index, it would pollute query results for other files. For example, a symbol definition temporarily disappearing from a header being edited would affect find-references results for every file that references that symbol. The global index only accepts stable state saved to disk, built through background indexing from disk files.

- **Is there a hash collision risk with content-addressed deduplication?**

  Theoretically SHA-256 collisions are possible, but the probability is negligible (on the order of 2^-128). In practice, treating SHA-256 collisions as "will not happen" is standard. Even if a collision occurred, the only consequence would be two different `FileIndex` entries sharing data — it would not cause a crash or data corruption.

- **Why FlatBuffers rather than Protocol Buffers or a custom format?**

  FlatBuffers allows queries to be executed directly on serialized data without deserializing first. For data like `MergedIndex` that may have thousands of shards, most shards are never accessed in a single session. FlatBuffers' zero-copy property makes loading a shard nearly free — only a memory-mapped file is needed, and only actually accessed data is read into memory. Protocol Buffers requires a full deserialization step, making it unsuitable for this on-demand loading model.

- **Why store `Occurrence` and `Relation` separately?**

  `Occurrence` is indexed by position — given an offset, binary search locates the symbol under the cursor, requiring position-sorted data. `Relation` is indexed by `SymbolHash` — given a symbol, look up all its relationships, requiring symbol-grouped data. Combining them into a single data structure would inevitably sacrifice efficiency in at least one of the two query patterns.

- **Why are cross-file queries performed in the main process rather than subprocesses?**

  clice uses a multi-process architecture where each open file is compiled in its own stateful subprocess. Cross-file queries (such as find-references) need to iterate over all open files' `Session` instances and aggregate their `FileIndex` results. If these Sessions were spread across different subprocesses, each query would require cross-process communication with multiple workers and then result aggregation — unacceptable in both latency and complexity. Instead, subprocesses send their `FileIndex` back to the main process after compilation, and the main process performs queries uniformly — it can access all open files' `FileIndex` entries as well as `ProjectIndex` and `MergedIndex`, completing the full query flow in a single process.

  This design also has a semantic consideration: query results for open files should reflect the editor's buffer state, not the disk state. Even if a file on disk has been modified by an external tool, as long as the editor has not sent a `didChange` notification, query results should be based on the version the editor holds. Centralizing all `FileIndex` entries in the main process makes this semantic invariant easier to maintain.

- **Why not use a database for index storage?**

  clice needs to persist multiple types of cache files: index shards, PCH, PCM, etc. PCH and PCM files are large (potentially hundreds of MB) but few in number (roughly proportional to the number of open files or modules), and have a simple lifecycle — create, read, delete when stale. There are no complex query or transaction requirements. The capabilities databases excel at (transactions, indexing, complex queries) are irrelevant for these files; filesystem management is sufficient.

  Index shards are the only part that could potentially benefit from a database: numerous (equal to the number of project files), small in size, and could benefit from atomic writes and automatic LRU eviction. But the current filesystem-based approach already handles these needs adequately. Whether the added complexity of introducing a database dependency is justified for this one use case needs to be evaluated when an actual bottleneck is encountered. For very large projects (tens of thousands of files), storing that many index shards in a single directory may create filesystem-level pressure; hierarchical storage or a lightweight database could be considered in the future.

## Known Limitations

- **Symbol table locality**. Currently all symbols (including function-local variables) are merged into `ProjectIndex`'s global symbol table. This causes a large number of symbols meaningful only within a single file to be stored globally, increasing hash table insertion overhead during merging and memory usage.

  The improvement direction is to introduce multi-level symbol tables — not only `ProjectIndex` should have a `SymbolTable`, but `MergedIndex` shards should have their own as well. The rule for determining which level a symbol belongs to is: a symbol belongs to the `SymbolTable` of the file where it is defined, provided it is internal (will not be referenced by other files). For example:

  ```cpp
  // utils.h
  inline int helper(int x) {
      auto temp = x * 2;    // temp is a local symbol of utils.h
      return temp + 1;
  }
  ```

  ```cpp
  // main.cpp
  #include "utils.h"
  static int counter = 0;    // counter is a local symbol of main.cpp

  int main() {
      counter = helper(42);
  }
  ```

  `temp` is defined in `utils.h` and will never be referenced by any other file — it should be in the `SymbolTable` of `utils.h`'s `MergedIndex` shard, not in `ProjectIndex`'s global symbol table. `counter` is a static variable in `main.cpp` and similarly should be in `main.cpp`'s `MergedIndex` shard. Note that although `temp` appears in a header file, it belongs to the header's `SymbolTable` rather than the including source file's `SymbolTable`, because it is defined in the header. Only symbols like `helper` and `main` that may be referenced cross-file need to enter `ProjectIndex`.

  The goal is to minimize `ProjectIndex`'s size and merging overhead, while avoiding duplicate storage of the same local symbol across multiple translation units.

- **Staleness detection precision**. The current staleness detection uses only mtime — re-indexing is triggered whenever a dependency file's mtime is later than the build timestamp. This produces unnecessary re-indexes in scenarios like `touch`, branch switching, or CI restores (file mtime changed but content is actually unchanged). The improvement direction is mtime + content hash dual-layer detection: the first layer uses mtime for a fast check — if unchanged, skip immediately (zero I/O); the second layer computes the content hash for files whose mtime changed — if the hash is unchanged, the content was not actually modified and can also be skipped. This approach is already used for compilation artifact staleness detection (PCH, AST); the index staleness detection should be aligned.

- **Fuzzy symbol search**. The current workspace symbol search (workspace/symbol) is a simple substring match that does a linear scan over all symbols in `ProjectIndex`. This is insufficient for large projects and does not support fuzzy matching.

  C++ symbol names have structure: `getSymbolHash` is camelCase, `get_symbol_hash` is snake_case, `std::vector<int>::push_back` has namespace qualification. When searching, users typically type abbreviations or fragments (e.g., `symhash`, `gSH`, `vec_pb`), expecting them to match the full symbol name. Substring matching cannot handle these queries.

  The improvement direction is to build a dedicated search index over symbol names. A tokenizer is needed to split symbol names by naming conventions (`getSymbolHash` → `[get, Symbol, Hash]`, `push_back` → `[push, back]`), then build an inverted index over the tokens. For example, trigrams (three-character groups) can be used as index keys, and at query time trigram intersections produce a candidate set that is then scored precisely. clangd's Dex index uses this trigram posting list approach and serves as a useful reference implementation. Another direction is to adopt a mature full-text search library, though the cost of introducing an external dependency needs to be evaluated.

- ~~**PCH-induced index split**~~ (resolved). When using PCH optimization, a file's compilation is split into two phases: the preamble is compiled into the PCH, then the PCH compiles the rest of the file. The PCH swallows everything before the preamble bound — the main file's compilation cannot see the headers' contents or the preamble region's own directives, and an open buffer's preamble may describe a compilation context that no disk translation unit was ever indexed with.

  This is now addressed by pairing each PCH with a preamble-state blob, produced by the same worker build while the freshly parsed preamble AST is still in memory (the only moment its index is obtainable without deserializing the whole PCH). The blob carries the preamble's full symbol index — every covered header plus the main file's preamble region — together with per-file content and line tables for position mapping, document links, inactive regions and the open conditional stack. It is stored, hit and evicted together with the PCH, opened as a memory-mapped FlatBuffer and queried without deserialization. Open files overlay these blobs onto index queries: set queries take the union with disk shards (identical rows collapse by location), single-answer queries prefer the overlay, and the buffer's preamble region resolves through the blob's main-file entry. This keeps navigation working for open files whose translation unit the background indexer has not (or cannot) index, and keeps results faithful to unsaved preamble edits.
