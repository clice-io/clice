# Index Design

## Background

Cross-file features of a language server — go-to-definition, find-references, call hierarchy, symbol search, etc. — all rely on a symbol index. The indexing system must address several core challenges:

**Performance at scale**: C++ projects can contain tens of thousands of files, each producing a large number of symbols after compilation. The indexing system must build, persist, and query the index within reasonable time and memory constraints.

**Incremental updates**: After a user modifies a file, the relevant index entries should update incrementally rather than re-indexing the entire project.

**Compilation-context awareness**: The same header file can produce different symbols under different compilation contexts. Existing solutions like clangd store only the result of the last compilation, causing users to see stale index data after switching contexts.

**Real-time feedback for open files**: Files the user is actively editing should be immediately reflected in query results, without waiting for background indexing to finish.

The following issues come up repeatedly in clangd's issue tracker:

- Go-to-definition jumps to the wrong location (when multiple translation units define identically named symbols)
- Background indexing is slow — large projects need tens of minutes for the initial index pass
- Symbol search results are incomplete or contain stale entries
- Template code in header files has different reference relationships under different instantiation contexts

## Design

### Three-Level Index Hierarchy

clice uses a three-level index structure, each level serving a different purpose:

```
TUIndex (compilation artifact)
    ↓ merge
ProjectIndex (global symbol directory) + MergedIndex (per-file sharded relation data)
    ↑ overlay
OpenFileIndex (real-time override for open files)
```

### TUIndex: Single-Compilation Output

A TUIndex is the raw index data produced by a single compilation. When a translation unit is compiled, SemanticVisitor traverses the AST, records all symbol occurrences and relations, and produces a TUIndex.

A TUIndex contains:

- **Per-file index data**: Each file involved in the compilation (the main file plus all included headers) has its own FileIndex, recording occurrences and relations within that file
- **Symbol table (SymbolTable)**: Maps symbol hashes to symbol names and kinds
- **Include graph**: Records include relationships between files

A TUIndex is transient data — it is no longer needed after being merged into ProjectIndex and MergedIndex. In the background indexing scenario, a TUIndex is generated and serialized in a worker process, then transmitted to the master process for merging.

### ProjectIndex: Global Symbol Directory

ProjectIndex is the global symbol directory, aggregating symbol information from all indexed files. It functions like a search engine's inverted index — given a symbol hash, you can quickly find its name, kind, and which files it appears in.

ProjectIndex stores:

- **Symbol table**: Symbol hash → symbol name, symbol kind, reference file bitmap (Bitmap)
- **Path pool**: Internalized mapping of file paths

**Key design point**: ProjectIndex does not store concrete location information (line and column numbers). Location information is stored in MergedIndex's per-file shards. ProjectIndex's role is that of a "directory" — it tells you which files a symbol exists in, then you look up the exact locations in the corresponding MergedIndex shard.

This separation keeps ProjectIndex compact. Reference file bitmaps are stored using Roaring Bitmap compression, which is highly memory-efficient.

### MergedIndex: Per-File Sharded Index

MergedIndex is the core storage layer of the indexing system. It maintains one shard per file in the project, storing all symbol occurrence positions and relation information for that file.

The core feature of MergedIndex is **compilation-context merging**. The same header file may be included by multiple source files, and each compilation may produce different symbol relations (e.g., due to conditional compilation, template instantiation, etc.). MergedIndex merges index data from these different compilation contexts into the same shard.

Merging uses content-addressed deduplication: a content hash is computed for each FileIndex produced by a compilation, and different compilation contexts with identical content share the same data. This avoids redundant storage when a header file is indexed multiple times.

MergedIndex shards also store file contents, used for converting byte offsets to LSP positions (line/column numbers).

### Symbol Identity: SymbolHash

Every symbol in the system is identified by a SymbolHash (`uint64_t`). It is generated from Clang's USR (Unified Symbol Resolution) — a canonical string representation of symbol identity that encodes namespace, class name, function signature, template parameters, and other information.

Key properties of SymbolHash:

- **Cross-file consistency**: The same symbol has the same SymbolHash across different files — this is the foundation for cross-file navigation
- **Compact and efficient**: A 64-bit integer is better suited as a DenseMap key than a variable-length USR string
- **Deterministic**: The same declaration always produces the same hash

### Symbol Relation Model

The core data in the index consists of **relations** between symbols. Each relation contains:

- **Relation kind (RelationKind)**: Definition, declaration, reference, weak reference, read, write, interface, implementation, type definition, base class, derived class, constructor, destructor, caller, callee, etc.
- **Location**: The source range where the relation occurs
- **Target symbol**: The other end of the relation (for type relations such as inheritance and calls)

Complementing relations are **occurrences** — records of a symbol's simple presence at a location, without a relation kind.

This distinction allows different queries to be served efficiently:

- **Go to definition**: Look up RelationKind::Definition
- **Find references**: Look up RelationKind::Reference
- **Call hierarchy**: Look up RelationKind::Caller / Callee
- **Type hierarchy**: Look up RelationKind::Base / Derived
- **Symbol under cursor**: Look up Occurrence

## Query Flow

### Cross-File Queries

Taking "go to definition" as an example, the query flow is:

1. **Locate the cursor symbol**: In the current file, find the Occurrence at the cursor position via byte offset to obtain the SymbolHash
2. **Query the symbol directory**: Look up all files referencing this SymbolHash in ProjectIndex
3. **Per-file relation query**: For each referencing file, determine whether it is currently open:
   - If the file **is open** (has a Session), use its OpenFileIndex to look up relation data, **skipping** the corresponding MergedIndex shard
   - If the file **is not open**, use the MergedIndex shard to look up relation data

Key point: For any given file, OpenFileIndex and MergedIndex are **mutually exclusive** — open files are queried only via OpenFileIndex, closed files only via MergedIndex. This is replacement, not layering. This ensures that query results for open files always reflect the latest buffer contents.

### Real-Time Override for Open Files

Each open file maintains an OpenFileIndex in its Session, storing index data from the most recent compilation. It contains the same types of data as a MergedIndex shard (relations and occurrences), but sourced from in-memory AST compilation results rather than background indexing.

Design principle: The open file's index is not written to the Workspace (does not affect global state); only after the file is saved does background indexing update the MergedIndex. This preserves the stability of the global index — half-typed code that may be incomplete should not pollute query results for other files.

During queries, if a referenced file is open, the query uses that file's OpenFileIndex instead of the MergedIndex shard — the two are mutually exclusive, not layered. This ensures the user sees results corresponding to the latest buffer contents.

## Background Indexing

### Scheduling Strategy

Background indexing is managed by the Indexer, which maintains a queue of files awaiting indexing. It uses the following scheduling strategy:

- **Idle-timeout batching**: Files are not indexed immediately upon entering the queue. Instead, an idle timer is set. When the timer fires, all files currently in the queue are processed as a batch. This avoids frequent small indexing runs.
- **Deduplication**: Files in the queue are deduplicated, preventing the same file from being indexed multiple times.
- **Pause/resume**: When the user initiates interactive requests (hover, completion, etc.), background indexing can be paused to prioritize user requests. Nested pause semantics are supported.

### Index Merging

After background indexing completes, the TUIndex merge process is:

1. Merge symbols from the TUIndex into ProjectIndex's global symbol table
2. Merge each file's FileIndex from the TUIndex into the corresponding MergedIndex shard
3. Update reference file bitmaps

Merging is incremental — only new data is processed; there is no need to rebuild the entire index.

## Serialization and Persistence

The index is serialized using FlatBuffers, supporting efficient on-disk persistence and lazy loading. FlatBuffers' zero-copy property allows the index to be accessed directly via memory mapping, avoiding the overhead of full deserialization.

Each MergedIndex shard is stored as an independent file on disk and loaded on demand. ProjectIndex is serialized as a whole since it is global data.

## Design Decisions and Trade-offs

**Why three levels instead of two?** The separation of ProjectIndex and MergedIndex is critical. If location information were also stored in ProjectIndex, its size would balloon, making it unusable as a lightweight "directory." Without ProjectIndex, every cross-file query would need to scan all MergedIndex shards to locate files containing a symbol — unacceptable for large projects.

**Why don't open-file indexes write to global state?** This is part of the two-layer state model (Workspace vs. Session). Unsaved edits may consist of incomplete code with syntax errors. Writing them to the global index would pollute query results for other files. Only stable state that has been saved to disk should affect the global index.

**Why use content-addressed deduplication?** Header files are frequently included by multiple source files, each compilation producing identical index data. Content addressing avoids storing N copies of the same data. When header content changes, old data is naturally replaced by the new version.

## Known Limitations and Future Directions

- **Symbol table locality**: Currently all symbols (including function-local variables, etc.) are merged into ProjectIndex's global symbol table. This causes a large number of symbols that are meaningful only within a single file to be stored globally, wasting memory and merge time. The improvement direction is to introduce file-level SymbolTables — local symbols stay in TUIndex / MergedIndex shards, and only truly cross-file-referenced symbols enter ProjectIndex.
- **Fuzzy symbol search**: Current symbol search uses simple substring matching. For scenarios like agent and workspace symbol queries, a more efficient fuzzy search index is needed (tokenization, prefix trees, n-gram indexes, etc.).
