# Design

This section documents clice's internal design and key engineering decisions.

## Documents

| Document | Description |
|----------|-------------|
| [Architecture](./architecture) | Multi-process model, Workspace/Session state, component responsibilities, async model |
| [Compilation](./compilation) | Pull-based compilation pipeline, PCH content-addressing, staleness detection, persistent cache |
| [Dependency Scanning](./dependency-scan) | Wave-based BFS startup scanner, include graph and module map construction |
| [Header Context](./header-context) | How header files borrow compilation context from their host sources |
| [Template Resolver](./template-resolver) | Pseudo-instantiation for dependent-type code completion |
| [Indexing](./indexing) | TUIndex → ProjectIndex → MergedIndex hierarchy, background indexing, persistence |

## Source Layout

| Directory | Responsibility |
|-----------|---------------|
| `src/server/` | Master process: MasterServer, Compiler, Indexer, WorkerPool, Workspace, Session |
| `src/compile/` | Clang-based AST building, diagnostic collection, clang-tidy, code completion |
| `src/feature/` | LSP feature implementations (hover, semantic tokens, folding, etc.) |
| `src/index/` | Symbol index: TUIndex, ProjectIndex, MergedIndex, serialization |
| `src/semantic/` | Template type resolution, AST visitor patterns, selection tree |
| `src/syntax/` | Fast dependency scanning, include resolution, preprocessor directive parsing |
| `src/command/` | Compilation database loading, command resolution, toolchain detection |
| `src/support/` | Shared utilities: JSON, URI, filesystem, async primitives |
