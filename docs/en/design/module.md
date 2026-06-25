# Module Compilation

## Background

C++20 introduced modules, the largest change to the C++ compilation model since the language's inception. Traditional C++ compilation is "compile each file independently" — each source file compiles to an object file, and files share declarations via header files. Modules break this independence: when a file `import`s another module, the imported module's **module interface unit** must be compiled first, producing a **precompiled module file (PCM)**, before the importing file can be compiled.

This introduces **compile-time dependency relationships** — a directed acyclic graph (DAG), where nodes are module files and edges are `import` relationships. Build systems (CMake, Ninja, etc.) naturally support this kind of DAG scheduling, but a language server faces fundamentally different challenges:

**Real-time requirements**: A build system can scan all files at once, build a complete dependency graph, and compile in topological order. A language server cannot — after a user opens a file, they expect editing feedback in milliseconds. Waiting for the entire module graph to compile is unacceptable.

**On-demand compilation**: The user has only opened a few files in the project; there is no need to compile the entire module graph. What the language server needs is "compile only the modules the current file requires" — a lazy, incremental compilation strategy.

**Cascading updates on file changes**: When the user modifies a module interface file and saves it, all PCMs of modules that directly or indirectly depend on it become stale. The language server must transparently cancel in-progress compilations, mark affected modules as dirty, and recompile them when next needed.

**Concurrency and races**: Multiple files may simultaneously need the same module's PCM. If two requests trigger compilation of the same module at the same time, duplicate compilation must be avoided while ensuring the later request can wait for the earlier compilation to finish.

**Cyclic dependencies**: Although C++ modules do not allow circular `import`s, users may temporarily introduce cycles during editing. The language server must detect this and report an error gracefully, rather than deadlocking.

In clangd, C++20 module support has long been in an experimental stage. Recurring issues in the community include: incomplete dependency resolution for module compilation, leading to missing PCM files; module files modified without dependents being recompiled, leading to stale diagnostics; lack of real-time tracking of module dependencies, requiring a manual server restart to get correct compilation results.

The root cause of these problems is the lack of a dedicated module compilation scheduling system. clice solves these problems at their source through CompileGraph — a reference-counted compilation DAG.

## Design

### Core Idea

CompileGraph is a compilation dependency DAG built at runtime. Each node (CompileUnit) represents a module file, and edges represent `import` dependency relationships. Unlike a build system's static DAG, CompileGraph is **lazily built** — a module's dependencies are resolved and added to the graph only when the module is actually needed.

CompileGraph's scheduling is based on **interest counting**: when a file needs a module, that module's reference count is incremented; when it is no longer needed, the count is decremented. A reference count reaching zero means no active request currently cares about this module, and its in-progress compilation can be cancelled to free resources.

### Key Concepts of CompileUnit

Each CompileUnit is organized around the following core concepts:

**Dependencies**: Each unit maintains forward dependencies (modules it `import`s) and reverse dependencies (modules that depend on it). Forward dependencies are obtained via lazy resolution; reverse dependencies are automatically populated during resolution. Reverse dependencies are the basis for cascading notifications when files change.

**Dirty state and generations**: The dirty flag indicates the unit needs recompilation. A generation counter is incremented on each file update, used to detect whether an asynchronous compilation result is stale — the compilation task captures the generation value at start and compares it upon completion.

**Compilation rounds**: Each compilation produces a round containing a completion event and a result. Waiters learn of completion through the event and decide their next action based on the result. A cancellation source is used to cancel the current round's compilation task.

**Reference count**: Tracks how many active requests care about this unit. When it reaches zero, compilation can be cancelled to free resources.

### Reference Counting and RAII Guards

Reference counting is the core scheduling mechanism of CompileGraph. It tracks "how many active requests currently care about a given compilation unit" and determines when compilation tasks are started and cancelled.

CompileGraph provides two RAII guards for managing reference counts:

**RefGuard (request-level guard)**: The entry point of a compilation request creates a RefGuard, which performs acquire (increment reference count) on all required compilation units. When the RefGuard is destroyed, it automatically performs release (decrement reference count). This ensures that even if a request is cancelled (coroutine frame destroyed), reference counts are correctly released.

**UnitGuard (compilation-round guard)**: Each compilation task internally uses a UnitGuard to maintain the compilation round's state. It is responsible for: acquiring reference counts on all direct dependencies after dependency resolution; upon completion or cancellation, publishing the result, clearing the compiling flag, releasing all acquired dependency references, and firing the completion event to notify waiters.

Using destructors rather than coroutine finally blocks for cleanup is a key design choice — cancellation is implemented via coroutine frame destruction, so cleanup logic must be placed in destructors to guarantee execution.

### Lazy Dependency Resolution

Dependency resolution is an expensive operation — it requires scanning the module file's contents and parsing `import` declarations. CompileGraph does not resolve dependencies at node creation time; instead, it calls resolve_fn for the first time the node needs to be compiled.

resolve_fn is provided externally (typically based on precise scanning of the module file) and returns the list of files that the module directly depends on. Resolution results are cached in the CompileUnit's dependencies, and the dependents lists are back-filled at the same time. Subsequent compilations reuse the cached dependency information unless a file update resets the resolved flag.

### Compilation Flow

CompileGraph provides two entry points:

**compile(path_id)** — compile the specified module and all its transitive dependencies:
1. RefGuard performs acquire on the target module
2. If the target module is not dirty, return immediately
3. If no compilation is in progress, start a compilation task
4. Wait for the compilation round to complete
5. Based on the result, return success, failure, or retry (retry if stale)

**compile_deps(path_id)** — compile all transitive module dependencies of the specified file, but not the file itself. This is used for ordinary source files (non-module files) — they are not part of the module DAG themselves but may `import` modules.

The internal flow of a compilation task (unit_body):
1. Resolve dependencies (ensure_resolved)
2. Check for self-cycles (self-importing)
3. Acquire reference counts on all direct dependencies
4. Recursively wait for all dependencies to finish compiling
5. Dispatch the actual compilation work to a stateless worker (dispatch_fn)
6. Check the generation counter — if the file was updated during compilation, the result is stale and the dirty flag is not cleared

### Zero-Reference Deferred Cancellation

When a compilation unit's reference count drops to zero, it means no active request currently cares about it. But cancelling immediately may be too aggressive — during dependency switching, the reference count might drop to zero and then increase again within the same event loop tick.

CompileGraph uses a strategy of deferring by one event loop tick: when the reference count drops to zero, it does not cancel immediately but instead schedules a deferred check. The deferred check runs on the next event loop tick — if the reference count is still zero at that point, cancellation actually proceeds. This avoids transient zero-references triggering unnecessary cancellation and restart.

### Generation Counter and Race Detection

In an asynchronous system, a compilation task dispatched to a worker process must wait for the result to come back. During this time, the user may have modified the file, meaning the file content has already changed. If the old compilation result were marked as "clean," it would lead to use of stale PCMs.

The generation counter solves this problem: the compilation task captures the current generation value at start, and upon completion, compares it against the current value. If they differ, a file update occurred during compilation, and the result is stale — the dirty flag is not cleared, and subsequent requests will trigger recompilation.

### File Updates and Cascading Propagation

When a module file is modified (via didSave), CompileGraph's update() method performs a cascading update:

1. Reset the resolved flag of the modified file (forcing the next compilation to re-resolve dependencies)
2. Clear old dependency edges
3. Mark as dirty and increment the generation counter
4. Traverse all transitive dependents along reverse dependency edges
5. For each affected dependent: cancel its compilation round, mark it as dirty, and increment its generation counter

update() returns the list of all modules marked dirty, allowing the caller to clear the corresponding PCM caches.

Note that update() does not modify reference counts — existing waiters retain their references. When they observe that the compilation result is "stale," they automatically retry, driving a new round of compilation.

### Cycle Detection

Although legal C++20 modules do not allow cyclic dependencies, they may be introduced temporarily during editing. CompileGraph detects wait cycles before waiting for a dependency's compilation to complete: it searches along the target's dependency chain, following only nodes that are currently compiling, checking whether the chain leads back to the current waiter. If a cycle is detected, it returns failure immediately, avoiding deadlock.

### Compilation Results and Retry Semantics

A compilation round has three possible results:

- **Success**: Compilation succeeded; the dirty flag is cleared
- **Failed**: Compilation failed (dependency failure, dispatch failure, or cyclic dependency). The dirty flag is not cleared — the next explicit request can retry
- **Stale**: Compilation was cancelled (due to a file update or reference count reaching zero). Waiters automatically retry a new compilation round

Stale retries are bounded — each update() increments the generation only once, so retries do not accumulate. Without a continuous stream of updates, retries terminate naturally.

### Structured Concurrency and Shutdown

All compilation tasks are managed through kota::task_group. task_group provides a structured concurrency guarantee — it waits for all child tasks to complete upon destruction. CompileGraph's shutdown() method achieves graceful shutdown by cancelling all tasks in the task_group and waiting for them to exit.

## Design Decisions and Trade-offs

**Why reference counting instead of a simple task queue?** A task queue cannot express the semantics of "no one needs this compilation anymore." In module compilation scenarios, a user might close the file that triggered a compilation while that compilation is still in progress; continuing to compile at that point wastes resources. Reference counting precisely tracks demand, giving cancellation decisions a solid basis.

**Why defer by one tick instead of cancelling immediately?** In an event loop model, multiple steps of an operation may complete within the same tick. If an old reference is released before a new one is established (transient zero-reference), immediate cancellation would cause unnecessary rework. Deferring by one tick ensures that reference handoffs within the same tick do not trigger cancellation.

**Why use a generation counter instead of locks?** The master process is a single-threaded event loop — there are no data races. The "races" here come from the timing of asynchronous operations. The generation counter detects "whether a file update occurred during compilation" with minimal overhead, without introducing the complexity of locks.

**Why is dependency resolution lazy?** Dependency resolution requires reading and scanning file contents, which is not cheap. If all dependencies were resolved at node creation time, opening a single file could trigger scanning of the entire module graph. Lazy resolution ensures that only modules actually needed for compilation are scanned.

**Why are failures non-sticky?** Module compilation failures are usually transient — the user is editing code, and syntax errors are the norm. Making failures persistent would prevent the user from getting correct results after fixing an error. Not clearing the dirty flag means the next request naturally retries.

## Known Limitations

- **Compilation graph memory accumulation**: Each compilation round creates a task in the task_group. Completed task frames are only reclaimed when the task_group is destroyed, not immediately upon completion. In a long-running server, a large number of completed task frames may accumulate.
- **Dependency resolution determinism**: The result of resolve_fn depends on the file's current content. If the file is modified between resolve and the actual compilation, the resolved dependencies may be inconsistent with those at compilation time. The generation counter can detect this, but it results in additional retries.
