# Architecture

clice uses a **multi-process** architecture: a master server coordinates multiple worker processes. The master handles LSP requests, manages state, and dispatches compilation tasks. Workers perform CPU-intensive work (parsing, indexing) in separate processes with memory limits.

The async runtime is provided by [kotatsu](https://github.com/clice-project/kotatsu) (`kota`), which wraps libuv with C++20 coroutines. clice never calls libuv directly.

## Source Modules

### `src/command/`

CLI parsing, compilation database loading, and toolchain detection. Implements the subcommand-based interface (`clice server`, `clice query`, etc.).

### `src/compile/`

Compilation unit abstraction. Wraps Clang's AST and preprocessor state, providing:
- `CompilationUnitRef` — unified access to source locations, directives, file content
- `DiagnosticID` — diagnostic classification (deprecated, unused, etc.)
- Preprocessor directive tracking (includes, embeds, module declarations)

### `src/feature/`

LSP feature implementations, each consuming a `CompilationUnitRef`:
- Code completion, hover, signature help
- Semantic tokens, inlay hints, document symbols
- Document links, folding ranges, formatting, diagnostics

### `src/index/`

Symbol indexing for cross-TU queries:
- `TUIndex` — per-translation-unit symbol data
- `ProjectIndex` — global symbol index with workspace-wide queries
- `MergedIndex` — per-file shards merging header contexts
- Include graph tracking

Serialized with FlatBuffers for efficient disk storage.

### `src/semantic/`

Semantic analysis beyond what raw Clang provides:
- `SemanticVisitor` — AST traversal recording symbol occurrences and relations
- `TemplateResolver` — pseudo-instantiation for dependent name resolution
- `find_target` — resolve nodes to their declaration targets
- Symbol kind/modifier classification

### `src/syntax/`

Lightweight syntax-level processing (no full AST needed):
- `Lexer` / `Scanner` — token-level utilities
- `DependencyGraph` — module/include dependency tracking
- Include path completion, module import completion

### `src/server/`

The server core, split into sub-modules:

#### `src/server/service/`
- `MasterServer` — top-level LSP server, routes requests to sessions/workers
- `LSPClient` — LSP protocol handler, capability registration
- `Session` — per-file state (active context, pending requests)

#### `src/server/compiler/`
- `Compiler` — compilation orchestration (PCH, PCM, index builds)
- `CompileGraph` — interest-counted module DAG with cancellation
- `Indexer` — background indexing, query resolution

#### `src/server/worker/`
- `StatefulWorker` — holds ASTs in memory, serves queries (hover, semantic tokens, etc.)
- `StatelessWorker` — ephemeral tasks (PCH/PCM builds, completion, signature help)
- `WorkerPool` — process lifecycle management

#### `src/server/workspace/`
- `Workspace` — project state, config, dependency graph
- `Config` — TOML/JSON config loading and validation

### `src/support/`

Utilities: logging, filesystem helpers, JSON serialization, string operations.

## Process Model

```
┌─────────────────────────────────────────────────┐
│                  Master Process                   │
│  event loop (kota) + LSP I/O + state management │
└────────────┬──────────────────┬─────────────────┘
             │ bincode IPC      │ bincode IPC
     ┌───────▼───────┐   ┌─────▼──────────┐
     │ Stateful       │   │ Stateless       │
     │ Workers (×2)   │   │ Workers (×N/2)  │
     │ hold ASTs,     │   │ build PCH/PCM,  │
     │ serve queries  │   │ completion,     │
     └───────────────┘   │ signature help  │
                          └─────────────────┘
```

Workers communicate with the master via stdin/stdout using bincode serialization. Each worker runs in its own process with a configurable memory limit (default 4 GB).
