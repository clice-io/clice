# Multi-Process Architecture

## Background

A C++ language server faces a unique engineering challenge: it relies on Clang to parse code, and Clang has many insufficiently tested code paths when handling incomplete or invalid code. Code in the editor is almost always incomplete — missing semicolons, unmatched parentheses, interrupted template instantiations — all scenarios that Clang was not designed to handle robustly. This leads to two serious problems:

**Memory leaks and bloat**: Clang is designed under the assumption that compilation is a short-lived, one-shot operation. The compiler process allocates memory, finishes compilation, and exits; the operating system reclaims everything. But a language server is a long-running process — each user edit triggers recompilation, and Clang's internally accumulated memory cannot be effectively reclaimed. In the clangd community, users frequently report that the server's memory usage reaches 10GB+ after running for several hours, eventually being killed by the system's OOM killer. Typical scenarios include: template-plus-macro combinations causing background indexing to exhaust 16GB of memory; legal code (such as a large array declaration `arr[50][6000000]`) causing OOM due to constant initialization checks.

**Crashes**: Incomplete code triggers various assertion failures and null pointer dereferences inside Clang. In clangd's issue tracker, numerous reports involve template instantiation crashes, enum declaration segfaults, corrupted namespaces in catch clauses, and similar scenarios. Because clangd uses a single-process architecture, any single crash terminates the entire language server, causing the user to lose all editing state — even if the crash is only related to one file.

**Priority inversion**: In a single-process / single-thread architecture, background indexing and foreground interactive requests (hover, completion, etc.) compete for the same thread. When background indexing is processing a large file, a user's hover request may have to wait several seconds for a response. Even with a thread pool, priority control between threads is difficult — the OS thread scheduler does not understand semantics like "the user is waiting for a hover result."

## Design

### Core Idea

clice solves the above problems through a multi-process architecture: each compilation task runs in an independent worker process, and the master process is responsible only for state management and request routing. Worker process crashes or memory leaks are isolated within process boundaries, affecting neither the master process nor other workers.

### Process Model

```text
Master Process (MasterServer)
├── Event loop (kota)
├── LSP / Agentic protocol handling
├── State management (workspace, sessions, invalidation)
├── Compile scheduling (task graph: PCH / PCM / AST / TU-run families)
├── Background indexing (index store + pump)
│
├── Stateful Worker Processes × N
│   ├── SF-0: holds AST, serves queries
│   ├── SF-1: holds AST, serves queries
│   └── ...
│
└── Stateless Worker Processes × M
    ├── SL-0: executes one-shot tasks
    ├── SL-1: executes one-shot tasks
    └── ...
```

All worker processes communicate with the master process via stdin/stdout pipes, using bincode serialization (an efficient binary format based on kotatsu's BincodePeer). Each worker's stderr is redirected to an independent log file for isolated debugging.

### Role of the Master Process

The master process is the system's coordinator, running a single-threaded event loop. It performs no CPU-intensive compilation work — all compilation is delegated to worker processes. The master process is responsible for:

- Receiving and routing LSP / Agentic requests
- Managing global state (Workspace, Session mappings)
- Scheduling compilation tasks and background indexing
- Monitoring worker process health
- Handling file change notifications and cascading updates

The single-threaded design means the master process requires no locks — all state modifications are executed serially within the event loop. This greatly simplifies state management complexity.

### Why Two Kinds of Worker Processes

Language server compilation tasks naturally fall into two categories whose differing characteristics require different scheduling strategies:

**Tasks that need to hold state**: After a user opens a file, they repeatedly query its hover information, semantic highlighting, symbol outline, etc. All of these queries are based on the same AST. If every query triggered recompilation, the latency would be unacceptable (a full compilation can take several seconds). Therefore, the compiled AST must be kept in memory for subsequent queries to reuse. These tasks require **file affinity** — each file is permanently bound to one process.

**One-shot tasks**: PCH builds, PCM builds, background indexing, code completion, signature help, formatting, etc. are all one-shot — no state needs to be retained after execution. These tasks require **load balancing and priority scheduling** — any available worker can execute them, but user-interactive tasks should take priority over background tasks.

Separating them into two kinds of workers allows each to use the most appropriate scheduling strategy.

## Stateful Worker Processes

The core responsibility of stateful workers is to hold compiled ASTs and serve query requests.

### File-Affinity Routing

Each open file is bound to a stateful worker via its path_id. This binding is stored in the master process's routing table. When a query request arrives for that file, the master process routes it to the corresponding worker based on the routing table.

The allocation strategy for new files is **least loaded** — the worker currently owning the fewest documents is selected. This ensures an even distribution of documents across workers.

### Compilation and Query Flow

1. The master process sends CompileParams (source text, compilation flags, PCH/PCM paths, etc.)
2. The worker compiles the AST and caches it in an in-memory DocumentEntry
3. Subsequent QueryParams (hover, semantic tokens, document symbol, etc.) reuse the cached AST
4. When file content changes (didChange), the master process sends a DocumentUpdate notification
5. On the next compilation request, the worker recompiles the AST with the new content

Requests for each document are serialized through a per-document mutex, ensuring that compilation and queries do not run concurrently on the same document.

### Document Eviction

When the number of documents held by a worker exceeds the limit, an LRU strategy evicts the least recently used document, freeing the memory occupied by its AST. The worker sends an eviction notification to the master process. Subsequent requests for that document trigger re-allocation to a worker and recompilation.

## Stateless Worker Processes

Stateless workers execute one-shot compilation tasks using a priority-aware scheduling strategy.

### Two-Level Priority Queue

- **High priority**: Work a user request is actively waiting on — interactive builds (code completion, signature help), formatting, and the PCH/PCM builds a foreground request depends on.
- **Low priority**: Background indexing runs, which can be deferred without affecting user experience.

Priority is a property of the dispatch, not of the task type: the same PCH build is dispatched High when a user request is blocked on it and Low when produced by background indexing.

High-priority tasks always take precedence in acquiring worker resources. Low-priority tasks are subject to a concurrency limit — the number of low-priority tasks running simultaneously has an upper bound, ensuring that workers are always available to handle high-priority requests. Background index runs additionally lower their OS process priority (via the `nice` system call), reducing their CPU impact on other system processes, including the editor itself.

### Dynamic Concurrency Control

The concurrency cap for low-priority tasks is dynamically adjusted based on system state:

**Foreground-aware budget**: While foreground activity is detected (user requests in flight), background work is capped at roughly 30% of the stateless workers; once the foreground goes idle, background may use full capacity. When foreground activity returns, workers are reclaimed quickly — running low-priority tasks hit cooperative cancellation checkpoints and requeue themselves, with a kill as the timeout fallback — so a burst of typing never waits behind a wall of index builds.

**Memory pressure feedback**: The master process periodically (every 3 seconds) checks available system memory. When available memory drops below 20% of total, the background allowance is decremented by 1; when available memory recovers above 40%, it is incremented by 1. Under severe pressure the allowance can drop all the way to zero, pausing background work entirely.

**Crash backoff**: When a stateless worker crashes, the background allowance is multiplied by 3/4 (multiplicative decrease). Crashes typically indicate encountering code that triggers a Clang bug; continuing at high concurrency risks more workers hitting the same problem. Multiplicative decrease is more aggressive than linear decrease, reducing system load more quickly.

This combined strategy — a foreground-first budget, linear adjustment for memory pressure, and multiplicative backoff for crashes — ensures graceful degradation under load rather than sudden OOM or cascading crashes.

## Crash Recovery

The core value of process isolation lies in crash recovery — containing a worker's failure within that process without affecting overall service.

### Stateful Worker Crash

1. The master process detects the worker exit via a monitoring task
2. It clears all bindings for that worker in the routing table
3. If the maximum restart count has not been exceeded, a new worker is launched
4. Subsequent requests for those documents are automatically routed to the new (or another available) worker and trigger recompilation
5. The user may experience a brief delay (the AST needs to be recompiled), but no editing content is lost — the text buffer lives in the master process's Session

### Stateless Worker Crash

1. The master process detects the exit
2. Crash backoff is triggered, lowering the concurrency cap
3. In-flight build requests are resent once to a healthy worker (build tasks are idempotent; a request that kills two workers in a row is treated as poisonous and surfaces its failure instead of retrying forever). Background index attempts are requeued by the scheduler
4. If the maximum restart count has not been exceeded, a new worker is launched

### Crash Budget and Revival

Each worker slot has a crash budget with exponential backoff between restarts. A stretch of healthy uptime resets the budget, so occasional crashes do not accumulate into a death sentence. A slot that exhausts its budget stops being restarted — but not permanently: after a cooldown period its budget is restored and the slot can be revived on demand. The pool therefore degrades temporarily under systemic failure (fewer workers, slower background indexing) and heals itself once the trigger passes, without ever interrupting the master.

## Design Decisions and Trade-offs

**Why multi-process instead of multi-threaded?** Threads cannot isolate crashes — a segfault in one thread terminates the entire process. Threads also cannot isolate memory leaks — all threads share the same address space. Multi-process adds IPC overhead (bincode serialization/deserialization) but provides genuine fault isolation. For a library like Clang, which has a known large number of crash and leak paths, process isolation is an engineering necessity.

**Why separate stateful and stateless workers instead of a unified worker pool?** Their scheduling strategies are fundamentally different. Stateful workers need file affinity (to ensure AST reuse); stateless workers need load balancing and priority queues. A unified pool would sacrifice either affinity (causing frequent recompilation) or scheduling flexibility (inability to differentiate priorities). Separating them lets each kind of worker focus on its own scheduling needs.

**Why is the master process single-threaded?** The master process has a light workload — it only performs routing and state management, not compilation. A single thread avoids all concurrency control complexity (locks, atomic operations, race conditions), while a coroutine-based event loop is sufficient for the I/O-intensive routing work.

## Known Limitations

- **No per-worker memory enforcement.** Worker memory usage is not capped or watermark-evicted; a pathological translation unit can grow a worker until the operating system's OOM killer intervenes, at which point normal crash recovery takes over. System-wide memory pressure only throttles background concurrency, it does not bound any single worker.

- **No hang detection.** Worker health is observed through process exit only. A worker stuck inside Clang (an infinite loop rather than a crash) is not detected or restarted automatically; the affected request waits until it is cancelled.

- **Synchronous startup.** Loading the compilation database, warming the toolchain cache, and the initial dependency scan run to completion before the server starts answering requests, and there is no progress reporting yet — on very large projects the server can appear unresponsive for a while right after startup.
