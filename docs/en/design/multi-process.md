# Multi-Process Architecture

## Background

A C++ language server runs Clang on code that is almost always incomplete, which Clang was never built to handle robustly, and keeps it running for days, where Clang assumes a short-lived process. Three problems follow:

- **Memory.** Clang's memory accumulates across recompiles and is not reclaimed. clangd users report 10 GB and more after a few hours, until the OOM killer ends the server.
- **Crashes.** Half-typed code trips Clang's assertions and null dereferences. In a single process, one crash ends the server and every open file with it.
- **Priority inversion.** Background indexing and the user's requests compete for the same threads, and no thread scheduler knows that the user is waiting for a hover.

## Design

Every compile runs in a worker process; the master process holds the state and routes requests. A worker's crash or leak stays inside the worker.

```text
Master process
├── event loop: LSP and control requests, state, invalidation
├── compile scheduling (the task graph) and background indexing
│
├── stateful workers × N    hold open files' ASTs, answer queries on them
└── stateless workers × M   one-shot work: PCH and module builds, index runs,
                            completion, signature help, formatting
```

Workers talk to the master over pipes in a binary encoding, and each logs to a file of its own.

The **master** is single-threaded: it compiles nothing, so routing and state fit one coroutine event loop, and its state needs no locks.

Work splits into two kinds because they schedule oppositely. Queries on an open file — hover, semantic tokens, the outline — reuse one AST, so they need **affinity**: a file stays with the stateful worker holding its AST. One-shot work keeps nothing, so it needs **load balancing and priorities**: any idle worker takes it, and the user's work goes first.

## Stateful Workers

A newly compiled file goes to the stateful worker holding the fewest. The master decides which ASTs are kept: about one per GB of memory, between 8 and 32; past that, the file asked about least recently is evicted. Eviction is not invalidation — nothing the compile read changed — so an evicted file still answers its whole-document requests (semantic tokens, folds, outline, links, diagnostics) from what its compile answered, and anything else recompiles it once, over its PCH, in tens of milliseconds.

## Stateless Workers

Each dispatch is **high** priority when a user request waits on it — completion, signature help, formatting, a PCH a hover needs — and **low** for the background index. Priority belongs to the dispatch, not the task: a background PCH build the user comes to wait for becomes high at once.

- **Foreground first.** With the user idle, background work keeps up to `[workers] background` workers busy. While the user is active, it drops to a single task: compiles share the processor's power budget, so each extra background compile slows the user's own. Running background tasks stop at their next cancellation point and requeue, so typing never waits behind index builds. Background runs also run at a lowered scheduling priority.
- **Memory.** While stateless work runs, the master samples the machine's free memory (and that of the cgroup clice runs in) every second and keeps a reserve free. A background task starts only when the memory left, less what the running tasks are still expected to grow, holds its expected peak: what its worker reached when the file was last indexed. When memory falls below the reserve, or would within two seconds, the newest background task is killed — it has the least done and the most growth ahead — and runs again later. One that does not fit even alone is not run again until the file changes, and `clice index` names it. Foreground work is never held back for memory.

## Crash Recovery

### Who Crashed It

A worker runs several requests at once, so its death alone does not say which one caused it. A dying worker therefore names, in its last line on stderr, the request its faulting thread was running. That request is blamed; every other request in flight was merely taken along and is resent once. A death that names nothing — the OOM killer, a signal — resends everything in flight once, and blames a request only after it dies twice as the worker's only request.

### What a Crash Pauses

Blame is kept per document and per kind of work — the compile, the preamble, the modules it imports, each query, completion, signature help, formatting — and pauses that kind of work on that document alone. A crashed compile leaves the document without an AST and a warning on its first line saying what crashed; a crashed hover pauses hover only. A request like one that just crashed on the same content answers empty instead of crashing a worker again.

Paused work never retries while the document sits still. It retries after the document or a file it includes changes, for up to three crashes in a row — half-typed code usually stops crashing once complete — and at once on a save, the explicit retry. Background indexing skips a file whose run crashes until the file changes. A crash while reading a cached PCH first rebuilds the PCH, since a corrupted file crashes the same way, and blames the document only if the rebuilt one crashes too.

### Restarts

A dead worker is replaced at once, its documents recompiling on their next request; their text lives in the master, so nothing typed is lost. Requests wait for a replacement rather than fail. Each slot has a crash budget, spent only by deaths no request is blamed for: a slot that exhausts it rests for thirty seconds and is then revived, so the pool degrades under systemic failure and heals without interrupting the master.

A request past its deadline — ten minutes for a build, two for anything else, counted in CPU time for stateless work — is taken for a hung worker, which is killed and the request blamed like a crash.

## Design Decisions

- **Why processes, not threads?** Threads isolate neither crashes nor leaks: a segfault ends the process, and all threads share one heap. Processes cost serialization between them, and buy real isolation from a library with as many crash and leak paths as Clang.
- **Why two kinds of workers?** One pool would give up either affinity, recompiling ASTs, or priorities, mixing the user's work with the background's.

## Known Limitations

- **Foreground work has no memory bound.** A pathological unit an open file compiles can grow its worker until the system's OOM killer steps in. Workers rank above the master for it, so the worker goes, not the server.
- **A small memory limit slows background work to one task**, and a unit needing more than the limit leaves is not indexed until it changes.
- **Startup reports no progress.** Loading the compilation database, warming the toolchain cache and the first dependency scan finish before the server answers; on a very large project it can look unresponsive for a while.
