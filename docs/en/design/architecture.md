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

`Workspace` holds everything derived from files on disk. It is the single source of truth for the compilation database, include/module dependency graphs, PCH/PCM caches, the project-wide symbol index, and a path interning pool.

Workspace is **never modified by unsaved buffer content**. The only mutation paths are: initialization at startup, `didSave` (which cascades invalidation), and background index merges.

### Session (Per-File, Volatile)

`Session` represents a single open file. Created on `didOpen`, destroyed on `didClose`. It holds the current buffer text, a generation counter for ABA prevention, AST dirty state, dependency snapshots for staleness detection, header context for header files, and a per-file symbol index for open-file queries.

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

- Pull-based: feature requests trigger compilation only when the AST is dirty or dependencies are stale.
- Resolves compile flags from the CDB, with header context fallback for header files.
- Builds or reuses PCH/PCM artifacts before sending compilation to a worker.
- Forwards queries (hover, semantic tokens, etc.) to the stateful worker holding the AST.
- Sends one-shot build requests (completion, signature help) to stateless workers.

### Indexer

Handles cross-file navigation queries and background indexing. Holds no persistent state -- reads from Workspace's project index and per-session file indices.

- Serves symbol lookup and relation queries (definition, references, call/type hierarchy).
- Runs background indexing on an idle timer: dispatches compilation to workers, merges results into the project-wide index.
- For open files, prefers the fresher per-session index over the project-wide one.

### WorkerPool

Manages worker process lifecycles and request routing.

- Spawns stateful and stateless workers at startup (counts are configurable).
- Routes stateful requests by path-affinity with LRU tracking and least-loaded fallback.
- Routes stateless requests by round-robin.

## Message Flow Example: Hover Request

1. Client sends `textDocument/hover` with a URI and position.
2. MasterServer resolves the URI to a path ID and finds the Session.
3. Compiler ensures the file is compiled (pull-based -- only rebuilds if the AST is dirty or dependencies are stale):
   - Resolves compile flags from the CDB (or via header context for headers).
   - Builds any missing PCM/PCH dependencies first.
   - Sends the compilation to a stateful worker. The worker builds the AST, returns diagnostics and index data.
   - If the user edited the file during compilation (generation mismatch), the result is discarded.
4. Compiler sends the hover query to the same stateful worker (which holds the AST in memory).
5. Worker runs the query against the resident AST and returns the result.
6. Result is relayed back to the client.

## Async Model

clice runs a **single-threaded event loop** powered by libuv, with C++20 coroutines (`co_await`) for structured concurrency. All components (MasterServer, Compiler, Indexer, WorkerPool) run on the same thread. There are no mutexes anywhere in the master process.

Blocking work (file I/O, clang compilation) happens in worker processes or on libuv's thread pool (via `et::queue()`). The master only performs lightweight coordination: dispatching IPC messages, updating state, and waiting on events.

Detached compile tasks are scheduled via `loop.schedule()` to make them immune to LSP `$/cancelRequest` cancellation. Feature requests that arrive during compilation simply wait on the shared `PendingCompile` event.
