# Compilation

## Incremental Parsing

Every time you modify code, clice must re-parse the file. clice uses a mechanism called preamble to implement incremental compilation. The preamble is a special case of [Precompiled Header](https://clang.llvm.org/docs/PCHInternals.html) — it builds the initial preprocessor directives (the `#include` block at the top of a file) into a PCH cache on disk. Later, when parsing, it loads the PCH directly, skipping the preamble section entirely.

For example:

```cpp
#include <iostream>

int main () {
    std::cout << "Hello world!" << std::endl;
}
```

The `iostream` header expands to ~20,000 lines. clice builds `#include <iostream>` into a PCH file, reducing subsequent re-parses from 20,000 lines to just 5 lines. Modifying the preamble section (adding/removing includes) or changing compile flags that affect preprocessing (`-D`, `-I`, `-std`) requires rebuilding the PCH.

### Staleness Detection

PCH freshness is checked via a two-layer scheme:

1. **Mtime check** — fast path: if no dependency has a newer mtime than the cached PCH, it's still valid.
2. **Content hash** — if mtime changed (e.g., a `touch` without real edits), hash the file contents. Only if the hash differs is the PCH rebuilt.

This avoids expensive rebuilds on timestamp-only changes (common with build systems that regenerate files in-place).

## Multi-Process Pipeline

Compilation tasks are distributed across worker processes:

- **Stateless workers** handle PCH builds, PCM builds (C++20 modules), completion, and signature help. These are ephemeral — no state persists between requests.
- **Stateful workers** hold parsed ASTs in memory and serve queries (hover, semantic tokens, inlay hints, etc.). Files are cached via LRU eviction.

The master process never performs compilation directly. It dispatches tasks and collects results via bincode IPC over stdin/stdout.

## C++20 Modules

Module compilation follows a dependency graph (`CompileGraph`). The master resolves module dependencies from the syntax-level `DependencyGraph`, then builds PCM files in topological order via stateless workers.

Each module unit has a `CompileUnit` in the compile graph with:

- A refcount (interest count) tracking how many downstream dependents need it
- A cancellation token that fires when the refcount drops to zero

When a file is modified, dependent module units are automatically recompiled in dependency order.

## Cancel Compilation

clice uses **interest-counted cancellation** to avoid wasting CPU on compilations that are no longer needed.

### The Problem

In a module DAG like `A → B → C`, if the user opens file C, compilation starts for A, then B, then C. But if the user closes C (or switches to another file) while A is still building, continuing to build B and C is pointless.

### The Mechanism

Each `CompileUnit` in the compile graph maintains a reference count:

- When a file needs a module's PCM, it increments (`acquire`) that module's refcount.
- When it no longer needs it (file closed, edit superseded), it decrements (`release`).
- When refcount reaches zero, a one-tick grace period fires. If still zero after the grace period, the compilation's cancellation token is triggered.

The grace period prevents thrashing — rapid open/close sequences don't cause redundant cancel-and-restart cycles.

### Supersede Logic

When a file is edited while its previous compilation is in-flight, the old build is "superseded" — its interest is released and a new build starts. The old worker task observes the cancellation token and aborts early.
