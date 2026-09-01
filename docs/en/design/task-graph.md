# Compile Task Graph and Module Compilation

## Background

C++20 introduced modules, changing the independent compilation model that C++ has used since its inception. In traditional C++, each source file compiles independently into an object file, sharing declarations between files via headers. Modules break this independence:

```cpp
// math.cppm — module interface unit
export module math;
export int add(int a, int b) { return a + b; }

// main.cpp — imports the module
import math;
int main() { return add(1, 2); }
```

Before compiling `main.cpp`, the module interface unit `math.cppm` must be compiled first, producing a precompiled module file (PCM). If a project has multi-level module dependencies — A imports B, B imports C — the compilation order must be C → B → A. This forms a directed acyclic graph (DAG), where nodes are module files and edges are `import` relationships.

Build systems (CMake, Ninja, etc.) are naturally suited for this kind of DAG scheduling: scan all files, build a complete dependency graph, compile in topological order. But a language server faces different challenges:

**Real-time requirements.** After a user opens a file that imports modules, they expect editing feedback within hundreds of milliseconds. Waiting for the entire module graph to compile is unacceptable. A language server needs a lazy, on-demand compilation strategy — compile only the modules the current file actually needs.

**Cascading file changes.** When the user modifies a module interface file and saves, all PCMs of modules that directly or indirectly depend on it become stale. The language server must detect this, cancel in-progress compilations, mark affected modules as dirty, and recompile them when next needed.

**Concurrency and cancellation.** Multiple files may simultaneously need the same module's PCM. The language server must avoid duplicate compilations and let later requests wait for earlier ones to finish. At the same time, when the user closes the file that triggered a compilation and no longer needs a module, the in-progress compilation should be cancellable to free resources.

**Temporary cyclic dependencies.** Although C++ modules do not allow circular `import`s, users may temporarily introduce cycles during editing. The language server must detect and report errors rather than deadlocking.

In clangd, C++20 module support has long been in an experimental stage, lacking dedicated design. [clangd/clangd#1293](https://github.com/clangd/clangd/issues/1293) is the clangd team's summary of the module support problem — concluding that "these are not just bugs that can be fixed, a design and new infrastructure is needed," and at the time "nobody has plans/availability to work on this soon." Years later, related issues continue to appear: [clangd/clangd#2569](https://github.com/clangd/clangd/issues/2569) lists critical missing features in modules (rename, find references, etc.); [clangd/clangd#2292](https://github.com/clangd/clangd/issues/2292) and [clangd/clangd#2497](https://github.com/clangd/clangd/issues/2497) report file-locking conflicts caused by clangd sharing PCM files with the build system — clangd holds the PCM open, preventing the build system from overwriting it.

The common root cause of these problems is the lack of a compilation scheduling system designed for language server use cases. clice addresses this with a general **compile task graph**: every expensive build product — module PCMs, preamble PCHs, document ASTs, one-shot batch runs — is a node in one graph, with lazy construction, on-demand compilation, real-time cooperative cancellation, and dependency cascading. Modules are the richest client of the graph, and this page uses them as the running example.

## Design

### One Graph, Several Node Families

The task graph itself is deliberately thin: it knows about nodes, edges, interest, rounds, and invalidation — and nothing about what a node builds. What a node _means_ is defined by its **family**:

- **PCM family**: one node per module, edges are `import` relationships. This is where all module-specific policy lives.
- **PCH family**: one node per preamble content key — files whose preambles are identical share one node and one build.
- **AST family**: one node per open document, producing the published feature results.
- **TU-run family**: one-shot translation-unit runs shared by background indexing and batch lint.

The rest of this section describes the graph's general contract; module-specific behavior is called out explicitly.

### Lazy Construction

Unlike build systems that scan all files and build a complete DAG before compilation, the graph is lazily constructed — a node is created only when a compilation request first reaches it, and each of its build rounds re-resolves that node's dependencies. When the user opens a file, only the module chain actually needed by that file is scanned and compiled, not the entire project's module graph.

Two module entry points exist:

- **Compile module**: Compile the specified module and all its transitive dependencies, producing a PCM file. Used for module interface units themselves.
- **Compile dependencies**: Compile all module dependencies of the specified file, but not the file itself. Used for ordinary source files — they are not part of the module DAG but may use modules via `import`.

An `import` whose provider cannot be resolved yet does not fail the graph: it becomes a **placeholder node** that is never run, only depended upon. When a provider appears later — the file is created, or a compilation database reload reveals it — the graph updates the edge and invalidates dependents, which then rebuild against the real module.

### Interest Counting

Interest counting is the graph's core scheduling mechanism, tracking "how many active requests currently care about a given node." It answers two questions: should compilation be started? Can compilation be cancelled?

When a request needs a node, the node's interest is acquired; when the request finishes or is cancelled, it is released. Interest reaching zero means no active request currently cares about this product.

Interest operates at two levels: request-level and dependency-level. Request-level interest means "this request is waiting for the product"; dependency-level interest means "this node's build is waiting for its direct dependencies to complete." The latter ensures a module is not prematurely cancelled when another module's build depends on it, even if the original request has been cancelled.

### Compilation Rounds

Each build attempt constitutes a round. Concurrent requests for the same node join the running round instead of spawning their own; waiters learn of completion through the round's outcome and decide their next action. A round has three possible outcomes:

- **Success**: product built, dirty flag cleared
- **Failed**: compilation error (dependency failure, cycle detection, etc.), dirty flag retained
- **Stale**: the round was superseded (file modified during compilation, or interest dropped), waiters automatically drive a new round

Failure is not sticky — retaining the dirty flag means that after the user fixes an error, the next request naturally triggers a retry without requiring a server restart.

### Cooperative Cancellation

Cancellation is advisory, never destructive. Dropping interest to zero (or superseding a round) raises a stop flag; the running build observes it at declaration-level checkpoints inside the compile and winds down early, and the round publishes a real outcome to its waiters. A running round is never torn down mid-flight — which means the commit sequence at the end of a build (write the artifact, then register its metadata) always runs to completion once started, so cancellation can never leave a half-registered product behind.

### Dirty State and Generation Counter

Each node uses a dirty flag to indicate that a rebuild is needed. The generation counter is a monotonically increasing value, incremented on each file update, used to detect asynchronous races: the build records the current generation at start and compares upon completion — a mismatch means the file was modified during compilation, and the result is stale.

### Dependencies

Module nodes maintain forward dependencies (modules they import) and reverse dependencies (modules importing them). Forward dependencies are obtained via lazy resolution; reverse dependencies are back-filled at resolution time. Reverse dependencies are the basis for cascading notifications on file changes — starting from the modified module, following reverse edges finds all affected modules.

The module name to file mapping is maintained by DependencyGraph (see [Dependency Scanning](dependency-scanning.md)) — the startup fast scan discovers all module declarations and builds a module name → file path registry. The PCM family uses this registry to resolve `import` statements to concrete file paths.

## Implementation

### Compilation Flow

The complete flow of a module compilation request:

```
Request enters
  │
  ├─ Acquire interest on the target module's node
  │
  ├─ Target not dirty? ──→ Return immediately (PCM available)
  │
  ├─ No round in progress? ──→ Start a build round
  │                             │
  │                             ├─ Lazily resolve dependencies (scan import declarations)
  │                             ├─ Check for self-cycle
  │                             ├─ Acquire interest on direct dependencies
  │                             ├─ Wait for each dependency to compile
  │                             ├─ Dispatch to worker process (produce PCM)
  │                             └─ Check generation counter ──→ Mismatch = Stale
  │
  ├─ Join the round and wait for its outcome
  │
  └─ Based on result: Success → return / Failed → error / Stale → retry
```

Lazy resolution uses the Clang preprocessor for precise scanning, which differs from the fast lexer-based scan used during startup [dependency scanning](dependency-scanning.md). The fast scan does not expand macros or evaluate conditionals, suitable for building a global overview of include relationships; precise scanning expands all preprocessor directives to obtain the file's actual module dependencies under its current compile command. Resolution runs at the start of every build round rather than being cached, so a changed compile command or import list is always seen by the next round.

> Module implementation units (those with `module X;` but no `export`) implicitly depend on their corresponding module interface unit. Precise scanning detects this and automatically adds the dependency.

### Deferred Zero-Interest Cancellation

When interest drops to zero, cancellation is not requested immediately; the check is deferred briefly — if interest is still zero at that point, cooperative cancellation proceeds.

This handles the scenario where a compilation request is superseded. When the user edits during compilation, a new request replaces the old one: the old request releases its interest (momentarily zero), then the new request re-establishes it. Immediate cancellation would unnecessarily wind down shared dependency builds; the deferral allows such handoffs to complete smoothly.

### Cascading Updates

When a module file is saved (didSave), the graph performs a cascading update:

1. Mark the saved file's node dirty, increment its generation counter, and supersede any round in flight
2. Traverse all transitive dependents along reverse edges — both the durable edges recorded by earlier rounds and the candidate edges of rounds still in flight; for each affected module: supersede its running round, mark dirty, increment generation
3. Invalidate the affected cached PCMs so nothing consumes a stale product

Forward edges are left untouched: the saved file's next round rescans its `import` declarations and replaces them, so an import removed by the edit stops cascading only after that rescan. Cascading updates do not modify interest — existing waiters retain theirs. Upon observing a Stale outcome, they automatically drive a new round.

### Cycle Detection

Before waiting for a dependency's compilation to complete, the graph checks for wait cycles: starting from the target node, it searches along the dependency chain, following only nodes currently being built, checking whether the chain leads back to the current waiter. If a cycle is detected, it returns failure immediately, avoiding deadlock.

### Structured Concurrency

All rounds run as tasks inside a task group owned by the graph, providing structured concurrency guarantees: shutdown stops intake, cancels cooperatively, and waits for frames to unwind, ensuring no dangling build tasks remain. Completed round frames are reclaimed eagerly as they finish.

### PCM Caching

PCM files are named by configuration, not by content: the module name plus a hash of the compiler version, working directory, source path and frontend-relevant flags, stored in a dedicated cache directory. This is fully isolated from build system artifacts, avoiding file-locking conflicts. Content changes are caught by the dependency snapshots described next, not by the name.

Cached PCMs are validated through their dependency snapshots: each dependency's identity and observed content version are checked against the shared file table (stat fast path, content hash confirmation — see [Incremental Compilation](incremental-parse.md)). Recompilation only occurs when dependency content has actually changed, avoiding unnecessary rebuilds caused by "touch without modification." Cache metadata is persisted alongside the index and restored on server restart, and Clang's own PCM validation remains as the final backstop.

### Integration with the Compilation Pipeline

Before a document AST or interactive build is dispatched, its dependency preparation acquires the needed PCM nodes — this is the first step of compilation preparation, executed before PCH construction. The import scan runs on the request's actual buffer text (including any synthesized suffix for header contexts), so an `import m;` the user has typed but not yet saved still gets its PCM built before the parse needs it.

## FAQ

- **Why interest counting instead of a task queue?** A task queue cannot express "no one needs this compilation anymore." When the user closes the file that triggered a compilation, continuing wastes resources. Interest counting precisely tracks demand, making cancellation decisions grounded — not based on timeouts or heuristics, but on whether any request is still waiting for the result.

- **Why defer zero-interest cancellation instead of cancelling immediately?** In an event loop model, multiple steps of an operation complete within the same tick. When a compilation request is superseded, the old interest is released and the new one established shortly after, with a momentary zero in between. Immediate cancellation would unnecessarily wind down shared dependencies — deferring the check ensures interest handoffs don't trigger cancellation.

- **Why cooperative cancellation instead of destroying the build task?** Destroying a coroutine frame mid-build makes every await point a potential exit, and the commit sequence at the end of a build (write artifact, register metadata) must never be split in half. Advisory stop flags let the build exit at well-defined checkpoints and always publish a real outcome, at the cost of slightly delayed cancellation.

- **Why a generation counter instead of locks?** The master process is a single-threaded event loop with no data races. The "races" to detect come from asynchronous timing — "was the file updated during compilation?" The generation counter answers this with minimal overhead, without introducing locks.

- **Why lazy dependency resolution?** Dependency resolution requires running the Clang preprocessor (precise scanning) on each module file, which is not cheap. If all module dependencies were resolved at startup, it would add to startup time. Lazy resolution ensures only modules actually needed for compilation are scanned — opening a file doesn't trigger scanning the entire module graph.

- **Why are failures not sticky?** Users are actively editing code — syntax errors are the norm. If failures were marked as persistent, users couldn't get correct results after fixing errors without restarting the server. Retaining the dirty flag lets the next request naturally trigger a retry.

- **Why store PCMs in a separate cache directory?** clangd shares PCM files with the build system, which causes file-locking conflicts — the language server holds the PCM open, preventing the build system from overwriting it (see [clangd/clangd#2292](https://github.com/clangd/clangd/issues/2292)). clice keeps its PCMs in its own cache directory, avoiding this problem. The trade-off is additional disk space usage and extra time for first-time compilation.

## Known Limitations

- **Dependency resolution determinism.** A time window exists between resolving dependencies and the actual compilation. If a file is modified during this window, the resolved dependencies may not match the actual dependencies at compilation time. The generation counter detects this and triggers retries, but at the cost of extra compilation overhead.

- **Brand-new module providers.** An `import` whose provider module has never been saved to disk cannot be built — there is no file to compile. Once the provider is saved it is discovered through the placeholder-node mechanism; an import chain that was previously unresolvable heals when its dependents are next edited or reopened, not spontaneously.

- **Graph state is in-memory.** Node and edge state does not survive a server restart; the graph is rebuilt lazily from the persisted caches and the dependency registry on the next run.
