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
├── State management (Workspace, Session)
├── Compilation scheduling (Compiler, Indexer)
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

- **High priority**: User-interactive tasks — code completion, signature help. These need immediate response; the user is actively waiting for results.
- **Low priority**: Background tasks — index building, PCH/PCM builds, formatting. These can be deferred without affecting user experience.

High-priority tasks always take precedence in acquiring worker resources. Low-priority tasks are subject to a concurrency limit — the number of low-priority tasks running simultaneously has an upper bound (`low_limit`), ensuring that workers are always available to handle high-priority requests.

Additionally, the OS process priority of low-priority tasks is lowered (via the `nice` system call), reducing their CPU impact on other system processes, including the editor itself.

### Dynamic Concurrency Control

The concurrency cap for low-priority tasks is dynamically adjusted based on system state:

**Memory pressure feedback**: The master process periodically (every 3 seconds) checks available system memory. When available memory drops below 20% of total, `low_limit` is decremented by 1; when available memory recovers above 40%, `low_limit` is incremented by 1. This linear adjustment smoothly reduces background load under memory pressure.

**Crash backoff**: When a stateless worker crashes, `low_limit` is multiplied by 3/4 (multiplicative decrease). Crashes typically indicate encountering code that triggers a Clang bug; continuing at high concurrency risks more workers hitting the same problem. Multiplicative decrease is more aggressive than linear decrease, reducing system load more quickly.

This combined strategy — linear adjustment for memory pressure plus multiplicative backoff for crashes — ensures graceful degradation under high load rather than sudden OOM or cascading crashes.

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
3. In-flight tasks on the crashed worker are lost; they are only redone if a later request or scheduling cycle requeues them
4. If the maximum restart count has not been exceeded, a new worker is launched

### Maximum Restart Count

Each worker slot has a maximum restart count. If the same slot crashes repeatedly (e.g., crashing immediately after each startup), it indicates a systemic issue and no further restarts are attempted to avoid an infinite loop. The master process continues running with a reduced worker count — functionality may degrade (e.g., slower background indexing) but will not be completely interrupted.

## Design Decisions and Trade-offs

**Why multi-process instead of multi-threaded?** Threads cannot isolate crashes — a segfault in one thread terminates the entire process. Threads also cannot isolate memory leaks — all threads share the same address space. Multi-process adds IPC overhead (bincode serialization/deserialization) but provides genuine fault isolation. For a library like Clang, which has a known large number of crash and leak paths, process isolation is an engineering necessity.

**Why separate stateful and stateless workers instead of a unified worker pool?** Their scheduling strategies are fundamentally different. Stateful workers need file affinity (to ensure AST reuse); stateless workers need load balancing and priority queues. A unified pool would sacrifice either affinity (causing frequent recompilation) or scheduling flexibility (inability to differentiate priorities). Separating them lets each kind of worker focus on its own scheduling needs.

**Why is the master process single-threaded?** The master process has a light workload — it only performs routing and state management, not compilation. A single thread avoids all concurrency control complexity (locks, atomic operations, race conditions), while a coroutine-based event loop is sufficient for the I/O-intensive routing work.
