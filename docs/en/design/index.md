# Index

clice maintains a persistent symbol index for cross-TU queries — find references, call hierarchy, workspace symbol search, and more. The index is built in the background and stored on disk using FlatBuffers serialization.

## Architecture

### Data Layers

- **TUIndex** (`src/index/tu_index.h`) — per-translation-unit symbol data produced by `SemanticVisitor` during compilation. Contains symbol hashes, occurrence locations, and relations (definition, reference, base, override, caller/callee). This is an ephemeral output merged into the persistent stores below.

- **ProjectIndex** (`src/index/project_index.h`) — global cross-TU symbol index. Maps symbol hashes to their definition locations, names, kinds, and aggregated relations across the entire project.

- **MergedIndex** (`src/index/merged_index.h`) — per-file shards that merge header contexts. A single header may be indexed through multiple host sources; the merged index reconciles these into a unified view.

### Indexer (`src/server/compiler/indexer.h`)

The `Indexer` class is the query + scheduling layer. It holds no index data itself — persistent data lives in `Workspace` (ProjectIndex + MergedIndex shards), and per-file unsaved-buffer data lives in `Session` (OpenFileIndex).

Responsibilities:
- Cross-file navigation queries (definition, references, hierarchy)
- Symbol search (`workspace/symbol`)
- Background indexing scheduling with idle timeout and deduplication
- Merging TUIndex results into persistent stores
- Disk save/load of index shards

## Background Indexing

After a file is compiled, its TUIndex is merged into the project-wide index. Background indexing runs during idle periods (configurable via `idle_timeout_ms`, default 3s):

1. Files are enqueued when opened, saved, or when their dependencies change.
2. An idle timer deduplicates rapid changes — indexing starts only after the timeout.
3. Tasks are dispatched to stateless workers with configurable concurrency.
4. Indexing is paused during user-initiated requests (hover, completion) via `ScopedPause`.
5. Progress is reported to the client via LSP `$/progress` notifications.

## Queries

The indexer supports these cross-TU queries:

| Query | Method |
|-------|--------|
| Go to definition | `query_relations(path, pos, Definition)` |
| Find references | `query_relations(path, pos, Reference)` |
| Call hierarchy (incoming) | `find_incoming_calls(hash)` |
| Call hierarchy (outgoing) | `find_outgoing_calls(hash)` |
| Type hierarchy | `resolve_hierarchy_item()` |
| Workspace symbol | Search across ProjectIndex |

For open files with unsaved changes, queries check the Session's OpenFileIndex first, then fall back to the persisted MergedIndex.

## Serialization

Index data is serialized with [FlatBuffers](https://flatbuffers.dev/) (`src/index/schema.fbs`) for:
- Zero-copy deserialization — index shards can be memory-mapped from disk
- Compact binary format — smaller than JSON/protobuf for symbol data
- Efficient partial reads — only load the shards needed for a query

## Symbol Identification

Symbols are identified by a 64-bit hash (`SymbolHash`) derived from Clang's USR (Unified Symbol Resolution) string. USR generation (`src/index/usr_generation.cpp`) produces a canonical identifier for each symbol that is stable across TUs.
