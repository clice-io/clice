# Server Architecture

clice uses a **multi-process architecture** where a single **Master Server** coordinates multiple **Worker** processes. This design isolates Clang AST operations (which are memory-heavy and may crash) from the main LSP event loop.

## Overview

```
┌──────────────┐   JSON/LSP    ┌────────────────┐   Bincode/IPC   ┌──────────────────┐
│   LSP Client │ ◄──────────► │  Master Server  │ ◄─────────────► │ Stateful Workers │
│   (Editor)   │    (stdio)    │                 │    (stdio)      │  (AST cache)     │
└──────────────┘               │  - Lifecycle    │                 └──────────────────┘
                               │  - Documents    │
                               │  - CDB          │   Bincode/IPC   ┌──────────────────┐
                               │  - Build drain  │ ◄─────────────► │ Stateless Workers│
                               │  - Indexing     │    (stdio)      │  (one-shot tasks)│
                               └────────────────┘                 └──────────────────┘
```

## Master Server

The master server (`src/server/master_server.cpp`) is the central coordinator. It runs a single-threaded async event loop and never touches Clang directly. Its responsibilities:

### LSP Lifecycle

The server progresses through these states:

1. **Uninitialized** — waiting for `initialize` request
2. **Initialized** — capabilities exchanged, waiting for `initialized` notification
3. **Ready** — workers spawned, workspace loaded, accepting requests
4. **ShuttingDown** — `shutdown` received, draining work
5. **Exited** — `exit` received, stopping the event loop

On `initialized`, the master:

- Loads configuration from `clice.toml` (or uses defaults)
- Starts the worker pool (spawns stateful + stateless processes)
- Loads `compile_commands.json` and builds an include graph
- Starts the background indexer coroutine (if enabled)

### Document Management

Each open document is tracked in a `DocumentState` with:

- Current `version` and `text` (kept in sync via `didOpen`/`didChange`)
- A `generation` counter to detect stale compile results
- Build state flags (`build_running`, `build_requested`, `drain_scheduled`)

When a document is opened or changed:

1. The include graph is re-scanned (via dependency directives)
2. The compile unit is registered/updated in the `CompileGraph`
3. A debounced build is scheduled

### Build Drain

The `run_build_drain` coroutine implements debounced compilation:

1. Wait for the debounce timer (default 200ms) to expire
2. Ensure PCH/PCM dependencies are ready via `CompileGraph`
3. Send a `compile` request to the assigned stateful worker
4. Publish diagnostics from the result (or clear them on failure)
5. If more edits arrived during compilation (`build_requested`), loop back to step 2

This ensures rapid typing doesn't trigger a compile per keystroke.

### Request Routing

Feature requests are split between two worker types:

**Stateful workers** (affinity-routed by file path):

- `textDocument/hover`
- `textDocument/semanticTokens/full`
- `textDocument/inlayHint`
- `textDocument/foldingRange`
- `textDocument/documentSymbol`
- `textDocument/documentLink`
- `textDocument/codeAction`
- `textDocument/definition`

**Stateless workers** (round-robin):

- `textDocument/completion`
- `textDocument/signatureHelp`

All feature responses use `RawValue` passthrough — the worker serializes the LSP result to JSON, and the master forwards the raw JSON bytes to the client without deserializing. This avoids bincode↔JSON conversion overhead and serde annotation conflicts.

## Worker Pool

The worker pool (`src/server/worker_pool.cpp`) manages spawning and communicating with worker processes. Each worker is a child process of the same `clice` binary, launched with `--mode stateful-worker` or `--mode stateless-worker`.

### Communication

Workers communicate with the master via **stdio pipes** using a **bincode** serialization format (via `kota::ipc::BincodePeer`). This is more compact and faster than JSON for internal IPC, while the master handles JSON for the external LSP protocol.

### Stateful Worker Routing

Stateful workers use **affinity routing**: each file is consistently assigned to the same worker so that the worker retains the cached AST. Assignment uses a **least-loaded** strategy for new files, with **LRU tracking** to manage ownership.

When a worker exceeds its document capacity (currently hardcoded at 16 documents), it evicts the least-recently-used document and notifies the master via an `evicted` notification.

### Stateless Worker Routing

Stateless workers use simple **round-robin** dispatch. Each request includes the full source text and compilation arguments, so any worker can handle it independently.

## Stateful Worker

The stateful worker (`src/server/stateful_worker.cpp`) caches compiled ASTs in memory. Key behavior:

- **Compile**: Parses source code into a `CompilationUnit`, caches the AST, and returns diagnostics as a `RawValue` (JSON bytes)
- **Feature queries**: Look up the cached AST and invoke the corresponding `feature::*` function (hover, semantic tokens, etc.), serializing the result to JSON
- **Document updates**: Received as notifications — the worker updates the stored text and marks the document as `dirty`, causing feature queries to return `null` until recompilation
- **Eviction**: LRU-based; evicts the oldest document when capacity is exceeded, notifying the master
- **Concurrency**: Each document has a per-document `kota::mutex` (strand) to serialize compilation and feature queries. Heavy work (compilation, feature extraction) runs on a thread pool via `kota::queue`.

## Stateless Worker

The stateless worker (`src/server/stateless_worker.cpp`) handles one-shot requests that don't benefit from cached ASTs:

- **Completion**: Creates a fresh compilation with `CompilationKind::Completion` and invokes `feature::code_complete`
- **Signature help**: Similar to completion, using `feature::signature_help`
- **Build PCH**: Compiles a precompiled header to a temporary file
- **Build PCM**: Compiles a C++20 module interface to a temporary file
- **Index**: Compiles a file for indexing (TUIndex generation — currently a stub)

All requests are dispatched to a thread pool via `kota::queue`.

## Compile Graph

The compile graph (`src/server/compile_graph.cpp`) tracks compilation unit dependencies as a DAG. It handles:

- **Registration**: Each file registers its included dependencies
- **Cascade invalidation**: When a file changes, all transitive dependents are marked dirty and their ongoing compilations are cancelled
- **Dependency compilation**: Before compiling a file, `compile_deps` ensures all dependencies (PCH, PCMs) are built first
- **Cancellation**: Uses `kota::cancellation_source` to abort in-flight compilations when files are invalidated

## Configuration

The server reads configuration from `clice.toml` (or `.clice/config.toml`) in the workspace root. If no config file exists, sensible defaults are computed from system resources:

| Setting                  | Default               | Description                                 |
| ------------------------ | --------------------- | ------------------------------------------- |
| `stateful_worker_count`  | CPU cores / 4         | Number of stateful worker processes         |
| `stateless_worker_count` | CPU cores / 4         | Number of stateless worker processes        |
| `worker_memory_limit`    | 4 GB                  | Memory limit per stateful worker            |
| `compile_commands_path`  | auto-detect           | Path to `compile_commands.json`             |
| `cache_dir`              | `<workspace>/.clice/` | Cache directory for PCH/PCM files           |
| `debounce_ms`            | 200                   | Debounce interval for recompilation         |
| `enable_indexing`        | true                  | Enable background indexing                  |
| `idle_timeout_ms`        | 3000                  | Idle time before background indexing starts |

String values support `${workspace}` substitution.

## IPC Protocol

The master and workers communicate using custom RPC messages defined in `src/server/protocol.h`. Each message type has a `RequestTraits` or `NotificationTraits` specialization that defines the method name and result type.

### Stateful Worker Messages

| Method                        | Direction    | Purpose                               |
| ----------------------------- | ------------ | ------------------------------------- |
| `clice/worker/compile`        | Request      | Compile source and return diagnostics |
| `clice/worker/hover`          | Request      | Get hover info at position            |
| `clice/worker/semanticTokens` | Request      | Get semantic tokens for file          |
| `clice/worker/inlayHints`     | Request      | Get inlay hints for range             |
| `clice/worker/foldingRange`   | Request      | Get folding ranges                    |
| `clice/worker/documentSymbol` | Request      | Get document symbols                  |
| `clice/worker/documentLink`   | Request      | Get document links                    |
| `clice/worker/codeAction`     | Request      | Get code actions for range            |
| `clice/worker/goToDefinition` | Request      | Go to definition at position          |
| `clice/worker/documentUpdate` | Notification | Update document text (marks dirty)    |
| `clice/worker/evict`          | Notification | Master → Worker: evict a document     |
| `clice/worker/evicted`        | Notification | Worker → Master: document was evicted |

### Stateless Worker Messages

| Method                       | Direction | Purpose                      |
| ---------------------------- | --------- | ---------------------------- |
| `clice/worker/completion`    | Request   | Code completion at position  |
| `clice/worker/signatureHelp` | Request   | Signature help at position   |
| `clice/worker/buildPCH`      | Request   | Build precompiled header     |
| `clice/worker/buildPCM`      | Request   | Build C++20 module interface |
| `clice/worker/index`         | Request   | Index a translation unit     |
