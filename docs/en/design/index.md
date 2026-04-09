# Index

clice maintains a multi-layered symbol index that supports cross-file navigation (go-to-definition, find references, call hierarchy, type hierarchy) and workspace symbol search. The index is designed for incremental updates and fast queries on large codebases.

## Index Hierarchy

The index is organized in four layers, from finest to coarsest granularity:

**TUIndex** — the raw indexing output for a single translation unit. Produced by `TUIndex::build()` from a compiled AST. Contains a `SymbolTable` (symbol metadata keyed by hash), per-file `FileIndex` maps (occurrences and relations), an `IncludeGraph` recording include structure, and a build timestamp. TUIndex is a transient structure: it is serialized by the worker process, sent back to the main server, and merged into the persistent layers.

**ProjectIndex** — the global symbol table across all indexed translation units. Lives in `Workspace::project_index`. Maintains its own `PathPool` for path interning (separate from the server's `PathPool`), a `SymbolTable` merging symbols from all TUs, and a path-id mapping (`indices`) that maps project-level path IDs. `ProjectIndex::merge()` takes a `TUIndex` and returns a path-id remapping vector so that TU-local path IDs can be translated to project-global ones.

**MergedIndex** — per-file binary index shards stored in `Workspace::merged_indices`, keyed by project-level path ID and wrapped in `MergedIndexShard`. Each shard stores occurrences, relations, and a copy of the file's source content (for position mapping). MergedIndex supports two query methods: lookup by byte offset (for cursor-to-symbol resolution) and lookup by symbol hash + relation kind (for cross-reference queries). The binary format is designed for zero-copy access from disk via `MemoryBuffer`; modifications are loaded into an in-memory `Impl` and serialized back on save.

**OpenFileIndex** — per-session, in-memory index for open files with unsaved changes. Lives in `Session::file_index`. Contains a `FileIndex`, a `SymbolTable`, the buffer content at index time, and a cached `PositionMapper`. OpenFileIndex is rebuilt on every successful compilation of the open file's buffer and is never merged into the project-wide index.

## What's Stored

**Symbols.** Each symbol is identified by a `SymbolHash` (uint64, derived from the USR). The `Symbol` struct stores the symbol's `name`, `SymbolKind` (function, class, variable, etc.), and a `Bitmap` of files that reference it.

**Relations.** A `Relation` connects a source location to a target symbol with a `RelationKind` bitmask. Relation kinds include definition, declaration, reference, base class, derived class, caller, callee, and others. Relations are stored per-file in `FileIndex::relations`, keyed by the source symbol's hash.

**Occurrences.** An `Occurrence` maps a source range to a target symbol hash. Occurrences are stored in a sorted vector within each `FileIndex` and are used for cursor-to-symbol resolution (finding what symbol the cursor is on).

## Dual-Source Queries

The `Indexer` class serves as the query layer. It holds no index data of its own — all data lives in `Workspace` (ProjectIndex + MergedIndex shards) and `Session` (OpenFileIndex).

For cursor-based queries, `resolve_cursor()` checks the session's `OpenFileIndex` first (if the file is open), then falls back to the `MergedIndexShard` from Workspace. This ensures that queries on open files reflect the latest unsaved edits, while closed files use the most recent disk-derived index.

For cross-file queries (references, call hierarchy, type hierarchy), the Indexer iterates over all `MergedIndexShard` entries in `Workspace::merged_indices`, collecting matching relations. For files that are currently open, it checks whether a session exists and skips the MergedIndex shard in favor of the fresher OpenFileIndex.

## Background Indexing

Background indexing is piggybacked on compilation. When a stateful worker compiles a file (building PCH, PCM, or AST), it also produces `TUIndex` data as a side effect. The serialized TUIndex is sent back to the main server, where `Indexer::merge()` deserializes it, merges symbols into `ProjectIndex`, and updates the affected `MergedIndexShard` entries.

The Indexer also maintains an explicit background queue (`index_queue`) for files that need re-indexing. `Indexer::schedule()` starts a debounced idle timer; when it fires, `run_background_indexing()` dispatches compilation requests to the worker pool. `need_update()` checks staleness by comparing file mtimes against the stored build timestamps in each MergedIndex shard.

## Persistence

The index is persisted to the `.clice/index/` directory:

- **`project.idx`** — serialized `ProjectIndex` containing the global symbol table and path pool. Loaded on startup by `Indexer::load()`.
- **`shards/<path_id>.idx`** — one binary file per `MergedIndex` shard, named by the project-level path ID. Only shards that have been modified since the last save (`need_rewrite()` returns true) are written.

On startup, `Indexer::load()` reads `project.idx` and iterates the `shards/` directory to reconstruct `Workspace::merged_indices`. This allows the server to serve cross-file queries immediately without re-indexing the entire project. Incremental updates are saved periodically and on shutdown via `Indexer::save()`.
