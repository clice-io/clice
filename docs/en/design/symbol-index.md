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

clice's index system is redesigned to address these problems: it separates the global symbol directory from per-file sharded relation data, merges different compilation contexts of the same file through content-identical variant deduplication, stores all index blobs in a single embedded database with zero-copy on-demand access, and overlays live data for open files to ensure query result freshness during editing.

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

### Index Hierarchy

clice's index is organized into layers, each with a different lifecycle and responsibility:

```
TUIndex        Raw artifact from one compilation, discarded after merging
    ↓ merge
ProjectIndex   Global directory: external symbols + which TUs contributed which files
Shard          Per-file variant storage (exact positions and relations), loaded on demand
    ↑ overlay
Live rows      The open buffer's latest compile results, owned by its published projection
```

**TUIndex** is the raw index data produced by compiling a translation unit, projected from the unified semantic map built during the compile. Since a compilation involves the main file and all included headers, `TUIndex` internally maintains separate per-file row sets. It also carries a symbol table (hashes to names and kinds) and the include graph of this compilation. `TUIndex` is transient data, discarded after being merged into the persistent indexes.

**ProjectIndex** is the global directory. It has two jobs: the global symbol table for **externally visible** symbols (`SymbolHash` → name, kind, reference-file bitmap, compressed as Roaring Bitmaps), and the record of which translation units contributed which files (the basis for variant liveness and reconciliation when TUs disappear). It does not store exact symbol positions — it tells you which files a symbol exists in; the positions live in the corresponding shard. This separation keeps `ProjectIndex` compact enough to reside in memory at all times.

**Shard** is the per-file storage unit and the layer that actually serves queries. Each compilation context that preprocesses the file differently contributes one **variant** — a self-contained, canonically encoded blob of the file's rows. Names that are local to the file (internal linkage, function locals, anonymous-namespace members) live in the shard's own local-name table; only external names enter the `ProjectIndex`. Shards are loaded on demand — most are never touched in a session.

**Live rows** for an open file come from its most recent in-memory compilation and are owned by the document's published projection. They are never written to global state — they only overlay queries, so results reflect the buffer the user is actually editing.

### Symbol Table Layering

Symbol metadata (name, kind) is looked up in layers: an open file's live data first, then the `ProjectIndex` global table for external names, then the shard's local-name table for file-local names. Storage follows visibility — a symbol that no other file can reference never pollutes the global table.

## Implementation

### Index Construction

`TUIndex` construction is a projection over the unified semantic map: one AST traversal per compilation records every interesting node, and the index projection emits `Occurrence` and `Relation` rows per file. After projection, each file's rows are canonicalized — deduplicated and sorted (occurrences by position for binary search, relations by kind and position for filtering) — so that the encoded blob is deterministic.

During construction, the main file's rows are kept separate from the headers'. This allows different treatment during merging — the main file is merged as the translation unit's own contribution, while headers are merged as context variants.

### Variant Deduplication by Content Identity

The core problem for shards is: the same header file is included by N source files, producing N row sets. If each were stored in full, storage would grow linearly with the number of translation units. But in practice, the vast majority of headers produce identical index data under different compilation contexts. Only headers like the `crypto.h` example above, affected by conditional compilation, produce different content under different contexts.

clice solves this by making **the encoded bytes themselves the identity**. Each variant is encoded canonically and deterministically — same rows, same bytes — and its identity (`RowsHash`) is the xxh3 hash of those bytes. When a new compilation contributes a variant:

1. Encode the file's rows into the canonical blob and hash it
2. If a variant with this identity already exists in the shard, the merge is pure bookkeeping — the contributing TU is recorded, nothing is decoded or rewritten
3. If it is a new identity, the blob is added as a new variant

In practice the bookkeeping-only path covers the overwhelming majority of merges, which is what makes full-project indexing cheap: re-indexing an unchanged header touches no shard bytes at all.

Which variants are **live** is controlled by variant masks derived from the contribution records: when a translation unit is removed or re-indexed, the variants it no longer vouches for drop out of the live set and are filtered from queries.

### Zero-Copy Storage

All index blobs — shards, manifests, the serialized `ProjectIndex` — live in a single embedded LMDB database. Blob encodings are canonical and self-describing; on load a blob is validated once and then used directly as a memory-mapped, zero-copy view. There is no deserialize-into-structs step on the read path.

At startup, only the `ProjectIndex` and the per-TU manifests (compact) are loaded. Shards are opened on demand, and most are never accessed in a single session.

### Per-TU Context

Context information — which files a translation unit saw and through which include tree — is stored once per translation unit in its **manifest**, not duplicated into every shard. Shards know only variants; the manifests know which TU contributed which variant of which file. Staleness and reconciliation decisions (a TU disappeared from the CDB, a command changed, a dependency changed) are driven from the manifests and the contribution records.

### Query Flow

Using "find references" as an example to illustrate the full cross-file query flow:

1. In the current file, use the cursor's byte offset to binary-search the `Occurrence` rows and obtain the `SymbolHash` of the symbol under the cursor
2. Look up the `SymbolHash`'s reference-file bitmap in `ProjectIndex` to get all files containing the symbol
3. Query each file in the list:
   - If the file is open, its live rows (from the latest in-memory compile) answer, provided they are current for the buffer
   - Disk data for an **open** file is consulted only when the buffer content is byte-identical to the content the shard indexed — an edited buffer never serves stale disk positions
   - Files that are not open answer from their shard's live variants
4. Aggregate all `Relation` rows found across files (filtering by the target `RelationKind`), convert to LSP positions, and return to the client

For open files whose preamble is compiled into a PCH, the paired preamble-state blob (see below) supplies the rows the PCH swallowed.

> Converting offsets to LSP positions requires line-start information for the file. Shards store the line table (and enough of the content) for this conversion, so it works even for files that are not open.

### Staleness Detection

Staleness detection determines whether a file needs to be re-indexed. Each indexed artifact records the identity and observed content version of its inputs; validation is delegated to the master's shared file table, which performs the same two-layer check used everywhere else — a (size, mtime) stat fast path, then content-hash confirmation with stamp repair (see [Incremental Compilation](incremental-parse.md)). Re-indexing triggers only when input content actually changed; command changes are caught separately through the entry identity hashes recorded in the manifests.

### Background Indexing Scheduling

Background indexing scheduling must balance index timeliness against interference with user interaction. The index module employs the following strategies:

- **Queue with idle delay**: Files that need indexing are added to a queue, and processing begins only after the editor has been idle for a configurable period. This avoids triggering index tasks during rapid editing.
- **Foreground-aware budget**: Background index runs are capped to a fraction of the worker pool while the user is active and expand to full capacity when the foreground goes idle; memory pressure further shrinks the allowance (see [Multi-process Architecture](multi-process.md)).
- **Result merging and persistence**: Each index task compiles a file and builds a `TUIndex` in a stateless subprocess. The result is serialized and sent back to the main process, which merges it into the project index and shards. Modified blobs are committed to the database in batched transactions so they can be loaded directly on the next startup.

## FAQ

- **Why separate `ProjectIndex` and shards instead of using a single unified index?**

  If position information were also stored in `ProjectIndex`, its size would balloon dramatically, making it impossible to keep in memory. Without `ProjectIndex`, every cross-file query would need to traverse all shards to locate files containing the symbol — in a project with tens of thousands of files, loading that many shards is unacceptable. `ProjectIndex` serves as a lightweight directory layer that first narrows the search scope to a handful of specific files, then precise lookup happens in the corresponding shards.

- **Why don't open-file indexes write to global state?**

  Buffer content being edited by the user may be incomplete code with syntax errors. If this temporary state were written to the global index, it would pollute query results for other files. For example, a symbol definition temporarily disappearing from a header being edited would affect find-references results for every file that references that symbol. The global index only accepts stable state saved to disk, built through background indexing from disk files.

- **Is there a hash collision risk with content-identity deduplication?**

  Variant identity is a 64-bit hash of the encoded blob, so collisions are astronomically unlikely but not cryptographically impossible. A collision would make two genuinely different variants share one stored copy — wrong rows served for one context — but cannot corrupt data structures or crash the server. The trade-off (a much cheaper hash on the hot merge path) is deliberate.

- **Why a custom canonical encoding instead of a general serialization framework?**

  Two reasons. First, zero-copy: blobs validate once and serve queries directly from mapped memory, which general frameworks support to varying degrees. Second — the deeper one — **byte identity is the merge currency**: because the encoding is a strict, deterministic function of the rows, "same content" and "same bytes" coincide, and deduplication becomes a hash comparison instead of a structural diff. A framework whose output can vary (field ordering, padding, versioned wire formats) would break that equivalence.

- **Why store `Occurrence` and `Relation` separately?**

  `Occurrence` is indexed by position — given an offset, binary search locates the symbol under the cursor, requiring position-sorted data. `Relation` is indexed by `SymbolHash` — given a symbol, look up all its relationships, requiring symbol-grouped data. Combining them into a single data structure would inevitably sacrifice efficiency in at least one of the two query patterns.

- **Why are cross-file queries performed in the main process rather than subprocesses?**

  clice uses a multi-process architecture where each open file is compiled in its own stateful subprocess. Cross-file queries (such as find-references) need to combine the live rows of all open files with the persistent index. If those live rows were spread across different subprocesses, each query would require cross-process communication with multiple workers and then result aggregation — unacceptable in both latency and complexity. Instead, subprocesses send their index rows back to the main process after compilation, and the main process performs queries uniformly — it can see all open files' live rows as well as the project index and shards, completing the full query flow in a single process.

  This design also has a semantic consideration: query results for open files should reflect the editor's buffer state, not the disk state. Even if a file on disk has been modified by an external tool, as long as the editor has not sent a `didChange` notification, query results should be based on the version the editor holds. Centralizing the live rows in the main process makes this semantic invariant easier to maintain.

- **Why an embedded database for index storage, but plain files for PCH/PCM?**

  They have opposite shapes. Index blobs are numerous (one shard per project file), individually small, and written in bursts during background indexing — exactly the profile where a single LMDB database wins: batched atomic commits, no per-file dirent pressure at scale, and memory-mapped reads that preserve the zero-copy path. PCH and PCM files are the opposite: few, large (hundreds of MB), memory-mapped directly by Clang itself, with a trivial create/read/evict lifecycle — the filesystem plus the artifact store's atomic-rename commits handles them well, and Clang could not consume them out of a database anyway. On filesystems where the database cannot operate safely (network mounts), index persistence is disabled rather than degraded.

## Known Limitations

- **Cross-TU queries for internal-linkage symbols.** File-local names are stored in shard local-name tables, but the cross-file relation query path currently reaches disk only through the external-symbol directory — so "find references" on a `static` function returns only what the live overlay can see. The storage layering is in place; the query path for local symbols is not yet.

- **Fuzzy symbol search**. The current workspace symbol search (workspace/symbol) is a simple substring match that does a linear scan over all symbols in `ProjectIndex`. This is insufficient for large projects and does not support fuzzy matching.

  C++ symbol names have structure: `getSymbolHash` is camelCase, `get_symbol_hash` is snake_case, `std::vector<int>::push_back` has namespace qualification. When searching, users typically type abbreviations or fragments (e.g., `symhash`, `gSH`, `vec_pb`), expecting them to match the full symbol name. Substring matching cannot handle these queries.

  The improvement direction is to build a dedicated search index over symbol names. A tokenizer is needed to split symbol names by naming conventions (`getSymbolHash` → `[get, Symbol, Hash]`, `push_back` → `[push, back]`), then build an inverted index over the tokens. For example, trigrams (three-character groups) can be used as index keys, and at query time trigram intersections produce a candidate set that is then scored precisely. clangd's Dex index uses this trigram posting list approach and serves as a useful reference implementation. Another direction is to adopt a mature full-text search library, though the cost of introducing an external dependency needs to be evaluated.

- ~~**PCH-induced index split**~~ (resolved). When using PCH optimization, a file's compilation is split into two phases: the preamble is compiled into the PCH, then the PCH compiles the rest of the file. The PCH swallows everything before the preamble bound — the main file's compilation cannot see the headers' contents or the preamble region's own directives, and an open buffer's preamble may describe a compilation context that no disk translation unit was ever indexed with.

  This is now addressed by pairing each PCH with a preamble-state blob, produced by the same worker build while the freshly parsed preamble AST is still in memory (the only moment its index is obtainable without deserializing the whole PCH). The blob carries the preamble's full symbol index — every covered header plus the main file's preamble region — together with per-file content and line tables for position mapping, document links, inactive regions and the open conditional stack. It is stored, hit and evicted together with the PCH, opened as a memory-mapped FlatBuffer and queried without deserialization. Open files overlay these blobs onto index queries: set queries take the union with disk shards (identical rows collapse by location), single-answer queries prefer the overlay, and the buffer's preamble region resolves through the blob's main-file entry. This keeps navigation working for open files whose translation unit the background indexer has not (or cannot) index, and keeps results faithful to unsaved preamble edits.
