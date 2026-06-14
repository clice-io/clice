# Header Context

## The Problem

The compilation database (`compile_commands.json`) only contains entries for source files, not header files. Since headers are included into source files, their meaning can change depending on the including context.

### Context-Dependent Headers

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

`a.h` has different states in `b.cpp` (defines `X`) and `c.cpp` (defines `Y`). A language server that treats `a.h` as an independent source file can only show one state.

### Non-Self-Contained Headers

```cpp
// a.h
struct Y {
    X x;
};

// b.cpp
struct X {};
#include "a.h"
```

`a.h` cannot be compiled standalone, but compiles correctly when included from `b.cpp`. clangd reports errors in such headers because it compiles them independently. This pattern appears frequently in libstdc++ and popular header-only libraries.

## clice's Solution

clice implements header context switching — when you navigate to a header file, it uses the source file you came from as the compilation context.

### How It Works

1. **Context resolution** (`Compiler::resolve_header_context`): When a header is opened, clice finds host source files that include it (via `DependencyGraph::find_host_sources`). It builds a chain of includes from the host source to the target header.

2. **Preamble injection**: All source content preceding the `#include` that pulls in the target header is written to a temporary preamble file. The header is then compiled with `-include <preamble>` injected into its flags. This gives the header the exact preprocessor state it would have when included from that source file.

3. **Automatic switching**: When you jump from a source file into a header (e.g., via go-to-definition or file open), clice automatically uses that source file's context. The context is stored per-session in `HeaderFileContext`.

4. **Manual switching**: Users can explicitly switch context via the `clice/switchContext` command, which presents a list of available host sources for the current header.

### Context Invalidation

When the host source's preamble changes (include order modified, macros added), the header context is invalidated. A content hash of the preamble determines staleness — if the hash changes, the context is rebuilt on next query.
