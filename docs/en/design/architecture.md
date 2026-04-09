# Architecture

## Overview

clice is a C++ language server built on a multi-process, event-driven architecture. A single **master process** orchestrates all state and communication, delegating heavyweight compilation and query work to a pool of **worker processes** over IPC.

```
                    Editor (LSP client)
                          |
                    JSON-RPC (stdin/stdout)
                          |
  +---------------------- | -----------------------+
  |                 MasterServer                    |
  |                                                 |
  |   Workspace (project-level, persistent)         |
  |   ┌───────────────────────────────────────┐     |
  |   │ CDB, DependencyGraph, CompileGraph    │     |
  |   │ PCH/PCM cache, ProjectIndex, PathPool │     |
  |   └───────────────────────────────────────┘     |
  |                                                 |
  |   Sessions (per-file, volatile)                 |
  |   ┌───────────────────────────────────────┐     |
  |   │ buffer text, AST state, deps snapshot │     |
  |   │ PCH ref, header context, file index   │     |
  |   └───────────────────────────────────────┘     |
  |                                                 |
  |   Compiler                Indexer               |
  |        |                      |                 |
  +--------|----------------------|-----------------+
           |                      |
     Bincode IPC (pipes)    Bincode IPC (pipes)
           |                      |
   ┌───────┴───────┐     ┌───────┴───────┐
   │   Stateful    │     │   Stateless   │
   │   Workers     │     │   Workers     │
   │               │     │               │
   │ Hold ASTs     │     │ One-shot:     │
   │ Path-affinity │     │  PCH/PCM      │
   │ LRU eviction  │     │  Completion   │
   │               │     │  Sig. help    │
   │ Queries run   │     │               │
   │ against AST   │     │ Round-robin   │
   └───────────────┘     └───────────────┘
```

## Multi-Process Model

Workers are child processes spawned by `WorkerPool` at startup. There are two kinds:

- **Stateful workers** hold ASTs in memory. Each open file is routed to the same worker via path-affinity (a `path_id -> worker_index` map with LRU tracking). Queries like hover, semantic tokens, and go-to-definition run against the resident AST. Routing uses a least-loaded assignment strategy when a file is seen for the first time.

- **Stateless workers** are one-shot: they receive a request (build PCH, build PCM, code completion, signature help), execute it, return the result, and discard all state. Dispatched via round-robin.

This separation exists for three reasons:

1. **Crash isolation.** A worker crash (e.g. from a malformed translation unit) does not bring down the server. The master can detect the failure and recover.
2. **Memory management.** Stateless workers release all memory after each task. Stateful workers can be bounded by a configurable memory limit (`worker_memory_limit`).
3. **Parallelism.** Multiple files can be compiled simultaneously across workers without any shared mutable state or locking.

Communication between master and workers uses Bincode-encoded IPC over pipes (not JSON-RPC). The master never runs clang itself.

## Two-Layer State Model

All server state is split into two layers with a strict isolation boundary.

### Workspace (Project-Level, Persistent)

`Workspace` holds everything derived from files on disk. It is the single source of truth for:

- **CompilationDatabase** (`cdb`): loaded from `compile_commands.json` at startup.
- **DependencyGraph** (`dep_graph`): include edges and module declarations, built by a BFS scan at startup.
- **CompileGraph** (`compile_graph`): C++20 module DAG for topological compilation ordering.
- **PCH cache** (`pch_cache`): keyed by file path_id, stores PCH path, preamble hash, bound, and dependency snapshot.
- **PCM cache** (`pcm_cache`, `pcm_paths`): keyed by module source path_id.
- **Module map** (`path_to_module`): reverse mapping from file to module name.
- **Project index** (`project_index`, `merged_indices`): symbol table and per-file index shards from background indexing.
- **PathPool**: interning table that maps file paths to compact integer IDs.

Workspace is **never modified by unsaved buffer content**. The only mutation paths are: initialization at startup, `didSave` (which cascades invalidation), and background index merges.

### Session (Per-File, Volatile)

`Session` represents a single open file. Created on `didOpen`, destroyed on `didClose`. Fields include:

- `text`: current buffer content (may differ from disk).
- `generation`: monotonic counter incremented on every `didChange`. Used for ABA prevention during compilation.
- `ast_dirty`: whether the AST needs rebuilding before serving queries.
- `compiling`: shared pointer to an in-flight compilation event. Concurrent queries wait on it.
- `pch_ref`: reference to a PCH entry in `Workspace.pch_cache` (path_id, hash, bound).
- `ast_deps`: `DepsSnapshot` from the last successful compile, used for staleness detection.
- `header_context`: resolved host source and synthesized preamble for header files.
- `file_index`: `OpenFileIndex` built from the latest compilation of this buffer.

**Isolation principle:** Sessions never write to Workspace. The only path from Session to Workspace is `didSave`, which tells Workspace to rescan the disk file and cascade invalidation. Sessions may _read_ from Workspace (PCH/PCM paths, module mappings, include graph) but all compilation results stay local to the Session.

## Component Responsibilities

### MasterServer

The top-level orchestrator. Owns Workspace, the sessions map, WorkerPool, Compiler, and Indexer. Responsibilities:

- Registers all LSP request/notification handlers.
- Manages the server lifecycle (`Uninitialized -> Initialized -> Ready -> ShuttingDown -> Exited`).
- Handles document lifecycle directly: `didOpen` creates a Session, `didChange` updates buffer text and increments generation, `didSave` cascades invalidation through Workspace, `didClose` destroys the Session and enqueues background indexing.
- Dispatches feature queries to Compiler or Indexer as appropriate.

### Compiler

Drives worker processes to build ASTs, PCHs, and PCMs. Holds no persistent state of its own -- reads from Workspace and Sessions, writes results back to Sessions.

- `ensure_compiled()`: pull-based entry point. Ensures a file's AST is up-to-date before serving any query.
- `ensure_pch()` / `ensure_deps()`: builds or reuses PCH and PCM artifacts.
- `forward_query()`: ensures compilation, then sends a query to the stateful worker holding the AST.
- `forward_build()`: sends a stateless build request (completion, signature help).
- `fill_compile_args()`: resolves compile arguments from CDB, with header context fallback.

### Indexer

Handles cross-file navigation queries and background indexing. Also holds no persistent state -- reads from `Workspace.project_index` and `Workspace.merged_indices`, and from `Session.file_index` for open files.

- Symbol lookup, relation queries (definition, references, call/type hierarchy).
- Background indexing: maintains a queue of files, schedules indexing on an idle timer, dispatches compilation to workers, merges results into Workspace's ProjectIndex.
- Skips files that are currently open (they have fresher data in Session.file_index).

### WorkerPool

Manages worker process lifecycles and request routing.

- Spawns stateful and stateless workers at startup (counts are configurable).
- Routes stateful requests by path-affinity with LRU tracking and least-loaded fallback.
- Routes stateless requests by round-robin.
- Provides `send_stateful()`, `send_stateless()`, and `notify_stateful()` as the IPC interface.

## Message Flow Example: Hover Request

1. Client sends `textDocument/hover` with a URI and position.
2. `MasterServer` resolves the URI to a `path_id`, finds the Session.
3. Calls `Compiler::forward_query(QueryKind::Hover, session, position)`.
4. `forward_query()` calls `ensure_compiled(session)`.
5. If `ast_dirty` or dependencies are stale, a detached compile task is launched:
   - `fill_compile_args()` resolves CDB flags (or header context).
   - `ensure_deps()` builds PCMs via CompileGraph and PCH via `ensure_pch()`.
   - `CompileParams` is sent to the stateful worker via `WorkerPool::send_stateful()`.
   - Worker builds the AST, returns diagnostics and TUIndex data.
   - On success: `ast_dirty` is cleared, `file_index` is updated, diagnostics are published.
   - On generation mismatch: result is discarded, `ast_dirty` stays true.
6. `forward_query()` sends `QueryParams` to the same stateful worker.
7. Worker runs the hover query against the resident AST, returns the result.
8. Result is relayed back to the client.

## Async Model

clice runs a **single-threaded event loop** powered by libuv, with C++20 coroutines (`co_await`) for structured concurrency. All components (MasterServer, Compiler, Indexer, WorkerPool) run on the same thread. There are no mutexes anywhere in the master process.

Blocking work (file I/O, clang compilation) happens in worker processes or on libuv's thread pool (via `et::queue()`). The master only performs lightweight coordination: dispatching IPC messages, updating state, and waiting on events.

Detached compile tasks are scheduled via `loop.schedule()` to make them immune to LSP `$/cancelRequest` cancellation. Feature requests that arrive during compilation simply wait on the shared `PendingCompile` event.
