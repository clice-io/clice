# Compilation Context

## Background

In C++, the same file can produce entirely different compilation results under different conditions. For source files, different compilation commands (`-std=c++17` vs `-std=c++20`, different `-D` macro definitions, etc.) lead to different ASTs. For header files, the situation is even more complex -- when the same header is included by different source files, the preceding `#include` and `#define` directives differ, potentially producing entirely different preprocessing results.

Yet no existing language server -- not just those for C++ -- formally addresses this problem. When a user opens a file in their editor, the language server simply picks a "default" compilation context to provide services. When that default choice is wrong, the user sees incorrect diagnostics, incorrect completion suggestions, and incorrect navigation targets.

Taking clangd as an example, the community has long struggled with numerous related issues:

- Headers display incorrect diagnostics after being opened, because clangd chose the wrong host source file
- In multi-configuration projects (e.g., supporting both Debug/Release or different platforms), users cannot select which set of compilation commands to use
- Non-self-contained headers lack proper context, leading to a flood of false positives

clice treats **Compilation Context** as a first-class citizen throughout its entire design, fundamentally solving these problems.

## What is a Compilation Context

A compilation context is "the complete set of conditions needed to correctly compile a file." Its meaning varies by file type:

### Compilation Context for Source Files

For source files (`.cpp`), the compilation context is simply their **compilation command** -- the compilation options read from the compilation database (CDB). A source file may correspond to multiple compilation commands (e.g., in multi-configuration projects), with each command representing a different compilation context.

This case is relatively straightforward: the user selects a compilation command, thereby selecting a compilation context.

### Compilation Context for Header Files

For header files (`.h`), there are two cases:

**Self-contained headers**: If a header can be compiled independently (it has its own CDB entry, or is protected by `#pragma once` / include guards and does not depend on external macro definitions), it can be treated like a source file and compiled directly using its own compilation command.

**Non-self-contained headers**: If a header depends on preceding context provided by the source file that includes it (earlier `#include` and `#define` directives, etc.), a compilation context must be **synthesized**. This synthesized context consists of:

- **Host source file**: A source file that includes the target header
- **Include chain**: The include path from the host source file to the target header
- **Prefix code (preamble)**: Everything in the host source file that appears before `#include "target.h"`

This is the concept of a **Header File Context** -- essentially a specialization of compilation context for the header file scenario.

## Design

### Compilation Context Resolution Flow

When a file needs to be compiled, its compilation context is determined by the following priority:

1. **User's explicit selection**: If the user has specified a compilation context via the extension protocol (`clice/switchContext`), use the user's selection
2. **Direct CDB lookup**: If the file itself has an entry in the compilation database, use that entry's compilation command
3. **Header context synthesis**: If the file is not in the CDB (the typical header file scenario), automatically synthesize a compilation context

### Header Context Synthesis

The header context synthesis process works as follows:

**Step 1: Find the host source file.** Using the DependencyGraph's reverse include graph, perform a BFS upward search for all source files that transitively include the target header. Select the first source file that has a CDB entry as the host.

**Step 2: Compute the include chain.** From the host source file to the target header, find the shortest path through the forward include graph. For example: `main.cpp -> utils.h -> math.h` -- if the target is `math.h`, the include chain is `[main.cpp, utils.h, math.h]`.

**Step 3: Synthesize the prefix code.** For each file in the include chain (except the final target file), read its contents, scan for the `#include` line that includes the next file, extract everything before that line, and add `#line` directives for accurate error location reporting.

For example, if `main.cpp` contains:
```cpp
#include <vector>
#define DEBUG 1
#include "utils.h"
int main() { ... }
```

And `utils.h` contains:
```cpp
#pragma once
#include <string>
#include "math.h"
void util_func();
```

Then the synthesized prefix code for `math.h` would be:
```cpp
#line 1 "main.cpp"
#include <vector>
#define DEBUG 1
#line 1 "utils.h"
#pragma once
#include <string>
```

**Step 4: Cache the prefix file.** Write the synthesized prefix code to a disk cache, using the xxh3 hash of its content as the filename, achieving content-addressed deduplication. The cache path is `{cache_dir}/header_context/{hash}.h`.

**Step 5: Inject into the compilation command.** Use the host source file's CDB compilation command, replace the source file path with the target header path, and inject the prefix file via the `-include` flag. The resulting compilation command looks like:

```
clang -std=c++17 -Iinclude -include cache/header_context/abc123.h -c math.h
```

This way, `math.h` is compiled as if it were at the correct position within the host source file, with the correct preprocessor state.

### Context Switching

clice supports user-initiated compilation context switching through LSP extension protocols:

- **Query available contexts (`clice/queryContext`)**: Lists all available compilation contexts for a file. For headers, this returns all possible host source files; for source files, it returns all matching CDB entries.
- **Query current context (`clice/currentContext`)**: Returns the currently active compilation context.
- **Switch context (`clice/switchContext`)**: The user selects a new compilation context. This clears all cached compilation artifacts for the file (PCH reference, header context, dependency snapshot) and marks the AST as dirty, triggering recompilation.

Context switching is a relatively heavyweight operation -- it causes recompilation. But this is the correct behavior: different contexts mean different compilation results.

### Interaction with PCH Optimization

PCH (precompiled headers) plays an important role in header contexts. A header's prefix code is essentially a preamble, which can be compiled into a PCH to accelerate subsequent compilations.

PCH files are named by the **hash** of the prefix content on disk, achieving file-level content-addressed deduplication -- identical prefix content will only be stored as a single PCH file on disk. However, PCH cache lookup currently uses the file's path_id as the key (each file has its own cache entry), and cross-file cache sharing is not yet implemented. Changing the cache key to the content hash for true cross-file PCH sharing is a planned improvement.

### Interaction Between Compilation Context and Indexing

The indexing system is inherently aware of compilation contexts. The same header file under different compilation contexts may produce different symbol relations (for example, a function might be defined through conditional compilation in one context but not exist in another). MergedIndex merges index data produced from the same file under different compilation contexts, allowing users to query cross-context symbol information through a unified view.

## Core Dependencies

- **DependencyGraph**: Provides forward and reverse include relationship queries; the foundation for determining host source files and include chains
- **CompilationDatabase**: Provides compilation commands for source files
- **PathPool**: Internalized file path identifiers
- **Session**: Stores the compilation context state for each open file (`header_context`, `active_context`)
- **Workspace**: Stores the PCH cache, shared across multiple Sessions

## Design Decisions and Trade-offs

**Why inject via `-include` instead of modifying file contents?** Using the `-include` flag lets Clang naturally handle the prefix code without modifying the target file's buffer. This preserves file content consistency, while `#line` directives ensure diagnostics point to the correct original file and line number.

**Why not "compile the host source file and stop at the include"?** This approach (compiling the host source file directly and setting a hook to stop when the target header is reached) is conceptually simpler, but has a fundamental problem: it does not support PCH optimization. Since the host source file is the compilation subject, it is difficult to perform fine-grained preamble splitting and PCH caching. For frequently edited files, PCH caching is critical to performance.

**Why choose the shortest include chain?** Among multiple possible include chains, the shortest one produces the least prefix code, meaning a smaller PCH and faster compilation. Additionally, a shorter chain typically represents a more direct inclusion relationship, making it a more intuitive default context choice.

## Known Limitations

- **Recursive includes**: If the include graph contains cycles (broken by include guards or `#pragma once`), prefix synthesis needs to handle deduplication correctly. The current dependency scanner already handles cycles, but the prefix synthesis path has not been fully validated for this scenario.
- **Determinism of host selection**: When multiple host source files are available, the current implementation selects the first one found. A better strategy might prioritize the host that is "closest" to the target header (e.g., a source file in the same directory), but this would require heuristics.
