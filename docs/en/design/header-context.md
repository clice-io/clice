# Header Context

For clangd to work properly, users often need to provide a `compile_commands.json` file (hereinafter referred to as CDB file). The basic compilation unit of C++'s traditional compilation model is a source file (e.g., `.c` and `.cpp` files), where `#include` simply pastes and copies the content of header files to the corresponding position in the source file. The aforementioned CDB file stores the compilation commands corresponding to each source file. When you open a source file, clangd will use its corresponding compilation command in the CDB to compile this file.

Naturally, there's a question: if the CDB file only contains compilation commands for source files and not header files, how does clangd handle header files? clangd treats header files as source files, and then, according to some rules, such as using the compilation command of the source file in the corresponding directory as the compilation command for that header file. This model is simple and effective, but it ignores some situations.

Since header files are part of source files, there will be cases where their content differs depending on the content that precedes them in the source file. For example:

```cpp
// a.h
#ifdef TEST
struct X { ... };
#else
struct Y { ... };
#endif

// b.cpp
#define TEST
#include "a.h"

// c.cpp
#include "a.h"
```

Obviously, `a.h` has different states in `b.cpp` and `c.cpp` - one defines `X` and the other defines `Y`. If we simply treat `a.h` as a source file, then only `Y` can be seen.

A more extreme case is non-self-contained header files, for example:

```cpp
// a.h
struct Y {
    X x;
};

// b.cpp
struct X {};
#include "a.h"
```

`a.h` itself cannot be compiled, but when embedded in `b.cpp`, it compiles normally. In this case, clangd will report an error in `a.h`, unable to find the definition of `X`. Obviously, this is because it treats `a.h` as an independent source file. There are many such header files in libstdc++ code, and some popular C++ header-only libraries also have such code, which clangd currently cannot handle.

clice supports **header context**: automatic and user-initiated switching of header file states, including non-self-contained headers. When you jump from `b.cpp` to `a.h`, clice uses `b.cpp` as the context for `a.h`. Similarly, jumping from `c.cpp` to `a.h` uses `c.cpp` as the context.

## Protocol API

clice exposes three LSP extension methods for header context management.

**`clice/queryContext`** — returns the list of host source files that include a given header.

```json
// Request
{ "uri": "file:///path/to/header.h", "offset": 0 }
// Response
{ "contexts": [{ "label": "main.cpp", "description": "/path/to/main.cpp", "uri": "file:///..." }], "total": 1 }
```

The `offset` parameter enables pagination (page size is 10). For source files that have their own CDB entries, this returns the available CDB configurations instead.

**`clice/currentContext`** — returns the active context override for an open file.

```json
// Request
{ "uri": "file:///path/to/header.h" }
// Response
{ "context": { "label": "main.cpp", "description": "/path/to/main.cpp", "uri": "file:///..." } }
```

Returns `null` for the `context` field if no override is set (i.e., automatic resolution is active).

**`clice/switchContext`** — sets a user-chosen context override for a header file.

```json
// Request
{ "uri": "file:///path/to/header.h", "context_uri": "file:///path/to/main.cpp" }
// Response
{ "success": true }
```

On success, this invalidates the session's cached header context, PCH reference, and AST deps, then marks the AST dirty so the next query triggers recompilation with the new context.

## Automatic Resolution

When a header file has no CDB entry, `resolve_header_context` automatically finds a suitable host source:

1. **Find host sources.** `DependencyGraph::find_host_sources()` performs a BFS upward through reverse include edges to find all root source files that transitively include the header.
2. **Select host.** If the session has an `active_context` override, that host is preferred. Otherwise, the first candidate with a valid CDB entry and a reachable include chain is chosen.
3. **Build include chain.** `DependencyGraph::find_include_chain()` performs a BFS forward from the host to the target header, returning the shortest path (e.g., `[host.cpp, intermediate.h, target.h]`).
4. **Synthesize preamble.** For each file in the chain except the target, the resolver reads the file content and extracts everything before the `#include` line that brings in the next file. Each segment is prefixed with a `#line` directive for correct diagnostics. The concatenated preamble is hashed (xxh3_64bits) and written to `.clice/header_context/<hash>.h`.

## Preamble Injection

`fill_header_context_args` takes the host source's CDB entry and modifies it for the header:

- Replaces the source file path with the header's path.
- Injects `-include <preamble_path>` into the compile flags (after `-cc1` for cc1 commands, after the driver otherwise).

This makes the compiler process the preamble (containing all preceding context from the include chain) before parsing the header, providing macros, types, and declarations that the header depends on.

## Caching

The resolved `HeaderFileContext` (host path ID, preamble path, preamble hash) is cached in `Session::header_context`. When the user calls `switchContext`, the cache is invalidated: `header_context`, `pch_ref`, and `ast_deps` are all reset, and `ast_dirty` is set to `true`. On the next compilation, `fill_header_context_args` detects the mismatch between `active_context` and the cached host and re-resolves accordingly.
