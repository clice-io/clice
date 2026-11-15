# Source Code Overview

clice is a C++ language server designed from the architecture up around problems earlier servers could not solve without a redesign:

- **Compilation context.** Every step of compiling, indexing and answering knows which context a file compiles in — which command, which includer for a header — and the user can see and switch it. See [Compilation Context](compilation-context.md).
- **Multiple processes.** A master process holds the state; worker processes run Clang, so a crash or a leak in Clang costs a worker, never the server. See [Multi-process Architecture](multi-process.md).
- **One build graph.** Every expensive product — a preamble's PCH, a module's PCM, an open file's AST, a unit's compile for its index — is a node of one graph that shares builds, cancels them and invalidates them along their dependencies. See [Compile Task Graph](task-graph.md).
- **Coroutines.** Asynchronous work is written as C++20 coroutines on the kotatsu library instead of callbacks.

## Layers

`src/` is one C++20 module in layers, listed here from the bottom up: each imports only the layers listed before it, except that `server` and `analysis` sit side by side above `project`, and `analysis` imports no `worker`.

| Layer      | What it holds                                                                                                                                                                                                   |
| ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `support`  | Utilities: logging, JSON, text, fuzzy matching, markup, failure records.                                                                                                                                        |
| `vfs`      | clice's view of the disk: one identity per file however a path spells it, one freshness check (a stat, then a content hash) shared by everything that asks whether a file changed, and the on-disk cache store. |
| `command`  | Compile commands: parsed once from the compilation databases, edited by the rules, completed by asking each command's compiler. See [Command Resolution](command-resolve.md).                                   |
| `syntax`   | What needs no AST: the lexer, the include and module dependency scan, the dependency graph, include resolution. See [Dependency Scanning](dependency-scanning.md).                                              |
| `compile`  | Clang behind one interface: a compilation's AST, diagnostics, tokens and directives, and clang-tidy.                                                                                                            |
| `semantic` | Analysis over the AST: one traversal per compilation that every feature queries, symbol identities equal across units, the [template resolver](template-resolver.md), documentation comments.                   |
| `index`    | The symbol index: a unit's index, the per-file shards, the project index and its database, and the queries over all of them. See [Symbol Index](symbol-index.md).                                               |
| `feature`  | Each language feature's answer, computed from an AST, the index or a document's text, with no state of its own.                                                                                                 |
| `settings` | `clice.toml` and the editor's settings over it, and the rules deciding what applies to each file.                                                                                                               |
| `worker`   | The worker processes and their pool: placement, priorities, memory, crashes and deadlines.                                                                                                                      |
| `project`  | A project as the disk describes it: its databases, members and commands, the build graph with its PCHs and modules, and its index. Unsaved buffers never change what a project says of the disk.                |
| `server`   | The language server: the open documents, which project serves each, what answers a request now, and the connection to the editor.                                                                               |
| `analysis` | The analyses behind `clice analyze` and `clice modularize`.                                                                                                                                                     |
| `driver`   | The `clice` binary's subcommands.                                                                                                                                                                               |

## Data Flow

```text
command (structured compile commands)
    ↓
project (task graph: PCH / PCM / AST / unit nodes) ──→ worker (processes)
    ↓ products
compile (drives Clang compilation)
    ↓
semantic (semantic map) ──→ index (unit index / shards / project index)
    ↓
feature (answers)
```

The server routes each request to the source that can answer it now — an up-to-date AST, or the index while the file compiles — gathers the feature's inputs and replies. Compilation is pulled by requests, never started by an edit alone (see [Incremental Compilation](incremental-parse.md)).
