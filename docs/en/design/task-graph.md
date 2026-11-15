# Compile Task Graph and Module Compilation

## Background

C++20 modules end independent compilation: before `main.cpp` can compile `import math;`, the interface unit of `math` must be compiled into a PCM, and a chain of imports must be compiled bottom up. Build systems handle this graph by scanning everything and compiling in order. A language server cannot:

- **It must answer in milliseconds**: compile only the modules the open file needs, not the project's whole graph.
- **Changes cascade**: saving a module interface makes every PCM above it stale, mid-build or not.
- **Requests overlap**: several files need the same PCM, which must build once; a build nobody needs any more should stop.
- **Code being typed can be wrong**: a cycle of imports must be an error, not a deadlock.

clangd's module support has lacked a design of its own for years — its own summary concluded that "a design and new infrastructure is needed" ([clangd#1293](https://github.com/clangd/clangd/issues/1293)) — and sharing PCMs with the build system locks files against it ([clangd#2292](https://github.com/clangd/clangd/issues/2292)). clice builds every expensive product on one **compile task graph**: lazily built, compiled on demand, cancelled cooperatively, invalidated along its dependencies. Modules are its richest client and the running example here.

## Design

### Nodes

The graph knows nodes, edges, interest, attempts and invalidation, and nothing of what a node builds. A node's kind gives its meaning:

- **PCM**: a module under a command it is built with; its edges are its imports, and the precompiled header its target reads.
- **PCH**: a preamble's content under its command — files whose preambles and commands agree share one node and one build.
- **AST**: an open document's compile, whose generation is the document's freshness.
- **Unit**: a translation unit compiled for its index or its lint.

**Whoever waits on a node carries how to build it.** A request, or a build waiting on a dependency, passes the node's work along; a waiter finding the node in need of a build with none running starts one with its work. Every waiter passes equivalent work, so it does not matter whose runs, and what a build needs lives exactly as long as someone waits for it — nothing is stashed for a build that may never start.

### Lazy Construction

A node is created when a request first reaches it, and each build resolves its dependencies anew, so opening a file scans and builds only the modules it reaches. An import no unit provides yet becomes a **placeholder** node, never built, only depended upon; when a provider appears, the placeholder is invalidated and its dependents rebuild against the real module.

### Interest

Interest counts how many active requests care about a node: a request acquires it while it waits and releases it when it finishes or is cancelled. A background build that a foreground request joins becomes foreground, and so do the builds it waits on. A build waiting on its dependencies holds interest in them too, so a module another module needs keeps building after the request that started it went away. At zero, the build is cancelled — after a short delay, since a request superseded by an edit releases its interest just before its successor takes it again.

### Attempts

Each build is an attempt; concurrent requests join the running one. It ends **succeeded**, **failed** (an error, a cycle) or **stale** (overtaken by a change, or by nobody wanting it). Failure is not sticky: the node stays dirty, so the next request after the user fixes the code checks it again — while the failed build's inputs are unchanged it fails again without running, and once one changes or an importer is saved it builds again.

A clean node's product can still vanish or go stale behind the graph's back — an evicted PCH, an evicted AST, a file changed before any event says so — so the work also carries a **check**, asked when a waiter reaches a clean node: the product **holds**; the product is **gone** but what it was built from is not, and only this node builds again; or the **inputs changed**, and the node and its dependents are invalidated.

### Cancellation and Generations

Cancellation is advisory: a stop flag the build checks at declaration boundaries inside the compile, after which the attempt still reports a real result. A build is never torn down mid-flight, so the end of a build — write the artifact, then register it — can never be left half done.

Each node has a generation, raised by every invalidation that reaches it. A build records it when it starts and compares when it ends: a difference means something it read changed meanwhile, and its result is stale. The master is single-threaded, so this is all the race detection it needs.

### Edges and Cascades

A node's edges are what its sources declare and what its last successful attempt waited on. A module's import scan declares its imports before it builds, and they stay its edges through a failed build, or fixing an import could never reach the module that failed on it. When a module file is saved, the graph walks reverse edges: every node above is marked dirty, and the running attempts that read what changed are overtaken. Before waiting on a dependency, the graph follows the chain of builds in progress and fails at once if it leads back to the waiter: a cycle is an error.

### Resolving Imports

The startup scan gives a registry of module names and the files declaring them (see [Dependency Scanning](dependency-scanning.md)); when several units declare one name, an import resolves to the nearest, and the importer is warned of the others. A build's own import scan runs the preprocessor on the unit as its command compiles it, skipping units that can import nothing and reusing a scan while the command and the files are unchanged — an open file's while its directive lines are, so typing elsewhere scans nothing. It runs on the buffer being edited, so an `import` typed but not saved gets its PCM built before the parse needs it.

A build system may compile one module several ways — once per set of importers' flags, once per build directory — and Clang reads a PCM only into a compile with matching language options. An import therefore reads the build its importer's compile matches: one under an entry of the importer's own database first, as the real build would, and the module's own command among those.

### PCM Cache

Each module keeps a record of its last build under each command: its key, what it read, and how it ended. The key names the build by configuration — the module, the compiler, the flags, the keys of the modules it imports — so a command change anywhere below a module changes its key; content changes are caught by checking what the build read with the shared file check (see [Incremental Compilation](incremental-parse.md)). Every build writes a new file in clice's own cache directory, so a rebuild never overwrites a PCM a compile is reading, and no file is shared with the build system. The records of successful builds are saved with the index, so a restart starts warm.

## FAQ

- **Why interest instead of a task queue?** A queue cannot say that nobody needs a build any more. Interest makes cancellation follow demand, not timeouts.
- **Why cooperative cancellation?** Destroying a build mid-flight makes every suspension point an exit, and the end of a build must never be split. A stop flag lets it leave at known points and always report a result.
- **Why does the waiter carry the work?** A build needs inputs only its requester has — a buffer's text, a lint run's arguments. Kept in the project for whichever build starts next, they would need rules for when to keep and drop them, and outlive their requests.
- **Why a separate PCM directory?** Sharing the build system's PCMs locks files against it. The cost is disk space and a first build of each module.

## Known Limitations

- **A module never saved to disk cannot be built**: there is no file to compile. Once it is saved, its placeholder is invalidated and the importers rebuild against it.
- **The graph lives in memory.** A restart rebuilds it lazily from the persisted records and the module registry.
