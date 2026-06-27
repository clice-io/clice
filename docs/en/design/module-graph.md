# Module Compilation

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

The common root cause of these problems is the lack of a module compilation scheduling system designed specifically for language server use cases. clice addresses this through CompileGraph — an interest-counted compilation DAG with lazy construction, on-demand compilation, real-time cancellation, and dependency cascading.

## Design

CompileGraph is a compilation dependency graph built at runtime. Each node (CompileUnit) represents a module file, and edges represent `import` dependency relationships.

### Lazy Construction

Unlike build systems that scan all files and build a complete DAG before compilation, CompileGraph is lazily constructed — a node's dependencies are resolved and added to the graph only when first touched by a compilation request. When the user opens a file, only the module chain actually needed by that file is scanned and compiled, not the entire project's module graph.

CompileGraph has two compilation entry points:

- **Compile module**: Compile the specified module and all its transitive dependencies, producing a PCM file. Used for module interface units themselves.
- **Compile dependencies**: Compile all module dependencies of the specified file, but not the file itself. Used for ordinary source files — they are not part of the module DAG but may use modules via `import`.

### Interest Counting

Interest counting is CompileGraph's core scheduling mechanism, tracking "how many active requests currently care about a given compilation unit." It answers two questions: should compilation be started? Can compilation be cancelled?

When a file needs a module, that module's reference count is incremented (acquire); when a request finishes or is cancelled, the count is decremented (release). A reference count reaching zero means no active request currently cares about this module.

Reference counts are managed through RAII guards: creating a guard automatically acquires, and its destructor automatically releases. This naturally aligns with coroutine cancellation semantics — cancellation destroys the coroutine frame, the destructor releases the reference count, with no extra cleanup code needed.

Interest counting operates at two levels: request-level and compilation-task-level. Request-level references mean "this request is waiting for a module's PCM"; compilation-task-level references mean "this module's compilation task is waiting for its direct dependencies to complete." The latter ensures a module is not prematurely cancelled when another module's compilation task depends on it, even if the original request has been cancelled.

### Compilation Rounds

Each compilation attempt constitutes a compilation round. Waiters learn of completion through a completion event and decide their next action based on the result. A round has three possible outcomes:

- **Success**: PCM produced successfully, dirty flag cleared
- **Failed**: Compilation error (dependency failure, cycle detection, etc.), dirty flag retained
- **Stale**: Compilation cancelled (file modified during compilation, or reference count reached zero), waiters automatically drive a new round

Failure is not sticky — retaining the dirty flag means that after the user fixes an error, the next request naturally triggers a retry without requiring a server restart.

### Dirty State and Generation Counter

CompileUnit uses a dirty flag to indicate that compilation is needed. The generation counter is a monotonically increasing value, incremented on each file update, used to detect asynchronous races: the compilation task records the current generation at start and compares upon completion — a mismatch means the file was modified during compilation, and the result is stale.

### Dependencies

Each CompileUnit maintains forward dependencies (modules it imports) and reverse dependencies (modules that import it). Forward dependencies are obtained via lazy resolution; reverse dependencies are back-filled at resolution time. Reverse dependencies are the basis for cascading notifications on file changes — starting from the modified module, following reverse edges finds all affected modules.

The module name to file mapping is maintained by DependencyGraph (see [Dependency Scanning](dependency-scanning.md)) — the startup fast scan discovers all module declarations and builds a module name → file path registry. CompileGraph uses this registry to resolve `import` statements to concrete file paths.

## Implementation

### Compilation Flow

The complete flow of a module compilation request:

```
Request enters
  │
  ├─ RAII guard acquires on target module
  │
  ├─ Target not dirty? ──→ Return immediately (PCM available)
  │
  ├─ No compilation in progress? ──→ Start compilation task
  │                                    │
  │                                    ├─ Lazily resolve dependencies (scan import declarations)
  │                                    ├─ Check for self-cycle
  │                                    ├─ Acquire on direct dependencies
  │                                    ├─ Wait for all deps to compile (parallel)
  │                                    ├─ Dispatch to worker process (produce PCM)
  │                                    └─ Check generation counter ──→ Mismatch = Stale
  │
  ├─ Wait for compilation round to complete
  │
  └─ Based on result: Success → return / Failed → error / Stale → retry
```

Lazy resolution uses the Clang preprocessor for precise scanning, which differs from the fast lexer-based scan used during startup [dependency scanning](dependency-scanning.md). The fast scan does not expand macros or evaluate conditionals, suitable for building a global overview of include relationships; precise scanning expands all preprocessor directives to obtain the file's actual module dependencies under its current compile command. Resolution results are cached and reused by subsequent compilations until reset by a file update.

> Module implementation units (those with `module X;` but no `export`) implicitly depend on their corresponding module interface unit. Precise scanning detects this and automatically adds the dependency.

### Deferred Zero-Interest Cancellation

When the reference count drops to zero, compilation is not cancelled immediately. Instead, a check is deferred by one event loop tick — if the reference count is still zero at that point, cancellation proceeds.

This handles the scenario where a compilation request is superseded. When the user edits during compilation, a new request replaces the old one: the old request releases its references (count momentarily drops to zero), then the new request establishes references within the same tick (count rises back). Immediate cancellation would unnecessarily terminate shared dependency compilations. The one-tick delay allows such reference handoffs to complete smoothly, avoiding disruption to in-progress shared compilations.

### Cascading Updates

When a module file is saved (didSave), CompileGraph performs a cascading update:

1. Reset the modified file's resolved flag — the next compilation will rescan dependencies, since the file may have added or removed `import` statements
2. Clear old forward dependency edges
3. Mark as dirty, increment the generation counter
4. Traverse all transitive dependents along reverse dependency edges; for each affected module: cancel its compilation round, mark dirty, increment generation
5. Return the list of all files marked dirty, so the caller can clear corresponding PCM caches

Cascading updates do not modify reference counts — existing waiters retain their references. Upon observing a Stale result, they automatically drive a new compilation round.

### Cycle Detection

Before waiting for a dependency's compilation to complete, CompileGraph checks for wait cycles: starting from the target node, it searches along the dependency chain, following only nodes currently being compiled, checking whether the chain leads back to the current waiter. If a cycle is detected, it returns failure immediately, avoiding deadlock.

### RAII Guards and Structured Concurrency

Coroutine cancellation in kotatsu means destroying the coroutine frame — code after suspension points never executes; only destructors of already-constructed objects are guaranteed to run. CompileGraph leverages this by placing all cleanup logic in two layers of RAII guards:

- **RefGuard** (request level): Holds root references from a request to modules. Releases reference counts on destruction when the request completes or is cancelled.
- **UnitGuard** (compilation round level): Manages all state for one compilation round. On destruction: publishes the outcome, clears the compiling flag, releases all acquired dependency references, and fires the completion event to notify waiters.

All compilation tasks are managed through kota::task_group, providing structured concurrency guarantees: shutdown cancels all tasks first, then waits for their frames to unwind, ensuring no dangling compilation tasks remain.

### PCM Caching

PCM files use content-addressed path naming — the filename is determined by the module name and a hash of the compilation arguments, stored in a dedicated cache directory. This is fully isolated from build system artifacts, avoiding file-locking conflicts.

PCM cache uses two-layer staleness detection: first comparing dependency files' modification times (mtime), then re-hashing content when times have changed. Recompilation only occurs when dependency content has actually changed, avoiding unnecessary rebuilds caused by "touch without modification." Cache metadata is persisted to `cache.json` on disk and can be restored on server restart.

### Integration with the Compilation Pipeline

CompileGraph is initialized during server startup. Before every file compilation, Compiler uses CompileGraph to ensure all module dependencies are ready — this is the first step of compilation preparation, executed before PCH construction.

For `import` statements that the user has added in the editor but not yet saved (which CompileGraph is unaware of), Compiler performs an additional buffer scan to identify new module dependencies and attempts to build the corresponding PCMs. This is a compensation mechanism that covers the experience when the user is editing but has not saved.

## FAQ

- **Why interest counting instead of a task queue?** A task queue cannot express "no one needs this compilation anymore." When the user closes the file that triggered a compilation, continuing wastes resources. Interest counting precisely tracks demand, making cancellation decisions grounded — not based on timeouts or heuristics, but on whether any request is still waiting for the result.

- **Why defer by one tick instead of cancelling immediately?** In an event loop model, multiple steps of an operation complete within the same tick. When a compilation request is superseded, the old reference is released and the new one established shortly after, with a momentary zero-reference in between. Immediate cancellation would unnecessarily terminate shared dependencies — deferring by one tick ensures reference handoffs within the same tick don't trigger cancellation.

- **Why a generation counter instead of locks?** The master process is a single-threaded event loop with no data races. The "races" to detect come from asynchronous timing — "was the file updated during compilation?" The generation counter answers this with minimal overhead, without introducing locks.

- **Why lazy dependency resolution?** Dependency resolution requires running the Clang preprocessor (precise scanning) on each module file, which is not cheap. If all module dependencies were resolved at startup, it would add to startup time. Lazy resolution ensures only modules actually needed for compilation are scanned — opening a file doesn't trigger scanning the entire module graph.

- **Why are failures not sticky?** Users are actively editing code — syntax errors are the norm. If failures were marked as persistent, users couldn't get correct results after fixing errors without restarting the server. Retaining the dirty flag lets the next request naturally trigger a retry.

- **Why store PCMs in a separate cache directory?** clangd shares PCM files with the build system, which causes file-locking conflicts — the language server holds the PCM open, preventing the build system from overwriting it (see [clangd/clangd#2292](https://github.com/clangd/clangd/issues/2292)). clice uses content-addressed isolated caching, avoiding this problem. The trade-off is additional disk space usage and extra time for first-time compilation.

## Known Limitations

- **Compilation task memory accumulation.** Each compilation round creates a task in the task_group. Completed task frames are only reclaimed when the task_group is destroyed, not immediately upon completion. In a long-running server, completed task frames accumulate over time.

- **Dependency resolution determinism.** A time window exists between resolving dependencies and the actual compilation. If a file is modified during this window, the resolved dependencies may not match the actual dependencies at compilation time. The generation counter detects this and triggers retries, but at the cost of extra compilation overhead.

- **Limited coverage for unsaved module dependencies.** CompileGraph builds dependency relationships from disk files. When the user adds an `import` in the editor without saving, Compiler compensates via buffer scanning, but if the imported module itself is not yet managed by CompileGraph (e.g., a new module file that hasn't been saved), the corresponding PCM cannot be built. The user needs to save the module interface file first, then save the importing file.
