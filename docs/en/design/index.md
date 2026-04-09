# Index

clice maintains a multi-layered symbol index that supports cross-file navigation (go-to-definition, find references, call hierarchy, type hierarchy) and workspace symbol search. The index is designed for incremental updates and fast queries on large codebases.

## Index Hierarchy

The index is organized in four layers, from finest to coarsest granularity:

**TU Index** — the raw indexing output for a single translation unit. Built from a compiled AST by the worker process. Contains symbol metadata (keyed by hash), per-file occurrence and relation maps, include graph structure, and a build timestamp. This is a transient structure: serialized by the worker, sent to the master, and merged into the persistent layers.

**Project Index** — the global symbol table across all indexed translation units. Maintains its own path interning pool (separate from the server's), a merged symbol table, and a mapping from project-level path IDs to per-file index shards. When a TU index is merged in, TU-local path IDs are remapped to project-global ones.

**Merged Index** — per-file binary index shards, keyed by project-level path ID. Each shard stores occurrences, relations, and a copy of the file's source content (for position mapping). Supports two query modes: lookup by byte offset (cursor-to-symbol) and lookup by symbol hash + relation kind (cross-reference). The binary format is designed for zero-copy access from disk; modifications are loaded into memory and serialized back on save.

**Open File Index** — per-session, in-memory index for open files with unsaved changes. Rebuilt on every successful compilation of the buffer and never merged into the project-wide index.

## What's Stored

**Symbols.** Each symbol is identified by a uint64 hash derived from the USR. Stores the symbol's name, kind (function, class, variable, etc.), and a bitmap of files that reference it.

**Relations.** Connects a source location to a target symbol with a bitmask of relation kinds: definition, declaration, reference, base class, derived class, caller, callee, and others. Stored per-file, keyed by the source symbol's hash.

**Occurrences.** Maps a source range to a target symbol hash. Stored in a sorted vector per file and used for cursor-to-symbol resolution.

## Dual-Source Queries

The Indexer serves as the query layer. It holds no index data of its own — all data lives in Workspace (project index + merged index shards) and Session (open file index).

For cursor-based queries, the open file index is checked first (if the file is open), then falls back to the merged index shard. This ensures that queries on open files reflect the latest unsaved edits, while closed files use the most recent disk-derived index.

For cross-file queries (references, call hierarchy, type hierarchy), the Indexer iterates over all merged index shards, collecting matching relations. For files that are currently open, it prefers the fresher open file index over the merged shard.

## Background Indexing

Background indexing is piggybacked on compilation. When a worker compiles a file (building PCH, PCM, or AST), it also produces index data as a side effect. The serialized index is sent back to the master, where it is merged into the project index and the affected per-file shards are updated.

The Indexer also maintains an explicit background queue for files that need re-indexing. A debounced idle timer dispatches compilation requests to the worker pool. Staleness is checked by comparing file mtimes against the stored build timestamps in each shard.

## Persistence

The index is persisted to the `.clice/index/` directory:

- **`project.idx`** — the global symbol table and path pool. Loaded on startup.
- **`shards/<id>.idx`** — one binary file per merged index shard. Only modified shards are written.

On startup, the project index and shard directory are loaded, allowing the server to serve cross-file queries immediately without re-indexing the entire project. Incremental updates are saved periodically and on shutdown.
