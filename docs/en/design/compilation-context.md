# Compilation Context

## Background

C++'s `#include` is a very primitive code reuse mechanism — it is essentially textual substitution. Unlike the module systems universally adopted by modern languages, it has no isolation: what a header file sees when included depends entirely on the preprocessor state before the inclusion point. This means **the same file can produce entirely different compilation results under different conditions**, and makes compiler caching optimizations much harder.

Source files produce different results due to different compilation commands (Debug/Release, different platforms); header files produce entirely different expansions depending on which source file includes them and what `#define` and `#include` directives precede the inclusion.

When a user opens a file in their editor, the language server must choose an appropriate compilation context to serve it — diagnostics, completion, and go-to-definition all depend on this context. Existing language servers (not just those for C++) have no formal concept of compilation context — they implicitly pick a context but give users no way to perceive or control this choice. When the context is wrong, compilation produces errors and the user sees a flood of false-positive diagnostics. clice is the first language server to introduce compilation context as a formal concept.

Taking clangd as an example, the lack of a compilation context concept has led to long-standing issues in the community:

- **Non-self-contained headers are not supported**. This is one of the earliest reported and still unresolved issues in the clangd community (clangd [#45](https://github.com/clangd/clangd/issues/45)). Since clangd has no concept of header context, it cannot restore the correct preprocessor state for non-self-contained headers, and users see a flood of meaningless errors when opening such files.

- **Guessing compilation commands for headers**. When clangd selects a compilation command for a header file, it mainly relies on path heuristics — guessing the "most relevant" source file from the compilation database (CDB) based on filename and directory structure, then borrowing that source file's compilation command. This guessing is unreliable and frequently picks the wrong file, leading to spurious diagnostics (clangd [#1505](https://github.com/clangd/clangd/issues/1505)).

- **Cannot select context in multi-configuration projects**. When a file appears multiple times in `compile_commands.json` (Debug/Release, different platforms), clangd picks only one entry, and the user has no control over which one is chosen (clangd [#681](https://github.com/clangd/clangd/issues/681)).

clice's design incorporates the compilation context framework from the start — this concept is naturally woven into the entire system. Users can not only see the currently active compilation context, but also query available contexts and switch between them.

## Design

A compilation context is **the complete set of conditions needed to fully restore the compilation state of a file**. Its meaning varies by file type.

For **source files**, the compilation context is their compilation command. A source file may correspond to multiple compilation commands in the CDB — common in multi-configuration projects. Each command represents a different compilation context, and which one is selected determines the file's behavior in the language server. For example, the same file can produce different code under Debug and Release:

```cpp
void initialize() {
    auto* conn = create_connection();
#ifndef NDEBUG
    conn->debug_label = "main";       // Only present in Debug builds
    log("connection created");
#endif
    conn->start();
}
```

Under Debug builds (`-O0`, without `NDEBUG` defined) and Release builds (`-O2 -DNDEBUG`), the visible symbols, diagnostics, and completion suggestions can all differ.

For **header files**, the compilation context falls into two cases. First, it helps to understand what "self-contained" means: a header file is self-contained if it does not depend on `#define` or `#include` directives provided by the includer and can compile independently on its own. The vast majority of modern C++ headers are self-contained — they `#include` all their dependencies and make no assumptions about the includer's state.

Self-contained headers can be treated like source files. However, headers are typically not in the CDB (the CDB only records compilation commands for source files), so even self-contained headers need to find a source file that includes them via the dependency graph and borrow that source file's compilation command (search paths, language standard, etc.). The key distinction is that self-contained headers do not need synthesized prefix code — borrowing the compilation command is sufficient.

> Strictly speaking, "self-contained" is a tendency rather than an absolute property. Even if a header can compile independently, the includer can still alter its semantics through `-D` flags or preceding `#define` / `#undef` directives. In practice, however, this is rarely done — a header designed to be self-contained should not depend on external preprocessor state.

Non-self-contained headers have compilation results that depend on the source file that includes them. A classic example is X macros:

```cpp
// errors.def — not self-contained, requires the includer to define the X macro
X(Ok,           0, "success")
X(NotFound,     1, "not found")
X(InvalidArg,   2, "invalid argument")
X(Timeout,      3, "operation timed out")
```

Different source files include the same `errors.def`, each defining a different `X` macro, expanding into entirely different code:

```cpp
// error_enum.cpp — generates an enum
#define X(name, code, msg) name = code,
enum ErrorCode {
#include "errors.def"
};
#undef X

// error_message.cpp — generates a string mapping
#define X(name, code, msg) case code: return msg;
const char* error_message(int code) {
    switch(code) {
#include "errors.def"
    }
}
#undef X
```

`errors.def` expands into completely different ASTs in the two source files — one is an enum definition, the other a switch statement. For such non-self-contained headers, the compilation context is called a **Header Context**, determined by two elements:

1. **Host source file**: A source file that directly or indirectly includes the header, providing the base compilation command
2. **Include position**: The specific position in the host source file's include tree where the header is included

The include position is important because the same header may be included multiple times within the same source file, with different preprocessor states each time (e.g., in the X macro pattern, a different macro is defined before each inclusion). All include positions in a source file can be flattened into an ordered sequence, with each position assigned a unique number. The host source file plus this number uniquely identifies a header context.

## How the Compilation Context is Determined

When a file needs to be compiled, the compilation context is automatically determined by the following priority:

1. **User selection takes priority**. If the user has actively selected a compilation context via `clice/switchContext`, that selection is used.

2. **Direct CDB lookup**. If the file has an entry in the compilation database, that entry's compilation command is used.

3. **Dependency graph lookup**. If the file is not in the CDB (the common case for headers), the DependencyGraph's include relationships are used to find a source file that includes it, and that source file's compilation command is borrowed. For self-contained headers, borrowing the compilation command is sufficient; for non-self-contained headers, prefix code must also be synthesized to restore the preprocessor state.

Beyond automatic resolution, clice also provides three LSP extension requests that let users explicitly query and switch compilation contexts:

**`clice/queryContext`** lists all available compilation contexts for a file. For headers, it returns all source files that can serve as hosts; for source files, it returns all matching CDB entries, using key flags from the compilation command (`-D`, `-std`, `-O`, etc.) as human-readable descriptions to distinguish them. Results support pagination. The returned context list is deduplicated — if two contexts are judged to produce the same compilation result, they are merged into a single entry. Otherwise, a widely-included header like `<vector>` could list thousands of contexts.

**`clice/currentContext`** returns the currently active user selection. If the user has not actively switched (i.e., the automatically selected default context is in use), it returns empty.

**`clice/switchContext`** switches to the user's chosen new context.

## Header Context in Practice

The header context is conceptually clear (host source file + include position), but given a header context, how to actually make Clang compile the header under the correct preprocessor state is an engineering problem that requires choosing an approach.

> The prefix synthesis discussed here only applies to headers the user has opened. Headers on disk that are not open do not need special handling — they are included and processed normally when each source file is compiled. The indexing system collects symbol information from headers as part of each source file's indexing pass, and MergedIndex merges the index data produced for the same header under different source files (see [Index Design](symbol-index.md) for the merging mechanism).

clice uses **prefix synthesis + `-include` injection**: based on the host source file and include position from the header context, it extracts all content before the target header along the include chain, synthesizes it into prefix code, writes it to a prefix file on disk, and then injects that file into the compilation command via Clang's `-include` flag. The core advantage of this approach is its natural compatibility with PCH optimization — the prefix file's content is the target header's preamble, which can be compiled into a PCH and cached. When the user subsequently edits the header body, the large volume of header inclusions in the prefix does not need to be reprocessed each time. Detailed rationale for this design choice is in the FAQ section below. PCH construction, caching, and invalidation mechanisms are described in [Incremental Compilation Design](incremental-parse.md).

The synthesis process has four stages. The following example illustrates the process. Suppose the project has these files:

```cpp
// main.cpp
#include <vector>
#define DEBUG 1
#include "utils.h"
int main() { ... }
```

```cpp
// utils.h
#pragma once
#include <string>
#include "math.h"
void util_func();
```

The user opens `math.h`, which is not in the CDB, so prefix code must be synthesized for it.

**Step 1: Find the host source file.** Using the DependencyGraph's reverse include graph, perform a BFS upward from `math.h` to find source files that transitively include it. In this example, `math.h` is included by `utils.h`, which is included by `main.cpp`, and `main.cpp` has a CDB entry, so it is selected as the host. If the user has specified a preferred host via `clice/switchContext`, that preference takes priority.

**Step 2: Compute the include chain.** From host `main.cpp` to target `math.h`, BFS on the forward include graph finds the shortest path: `[main.cpp, utils.h, math.h]`.

**Step 3: Synthesize the prefix code.** Along the include chain, for each file (except the final `math.h`), read its content, find the `#include` line that includes the next file, and extract everything before that line. Each fragment is prefixed with a `#line` directive to ensure diagnostics point to the correct original positions. The synthesized prefix code for this example is:

```cpp
#line 1 "main.cpp"
#include <vector>
#define DEBUG 1
#line 1 "utils.h"
#pragma once
#include <string>
```

The content after `#include "utils.h"` in `main.cpp` (`int main() { ... }`) is truncated, and the content after `#include "math.h"` in `utils.h` (`void util_func();`) is also truncated. This prefix code restores the preprocessor state that `math.h` would see in the original compilation: `<vector>` and `<string>` have been included, and `DEBUG` is defined as 1.

The prefix code is written to a disk cache, named by the xxh3 hash of its content, so identical content is stored only once.

**Step 4: Inject into the compilation command.** Using the host `main.cpp`'s CDB compilation command, replace the source file path with `math.h` and inject the prefix file via the `-include` flag:

```bash
clang -std=c++17 -Iinclude -include cache/header_context/a1b2c3d4.h math.h
```

Clang processes the `-include` prefix file first, then compiles `math.h`. The effect is that `math.h` is compiled as if at the `#include "math.h"` position in `main.cpp`, with the correct preprocessor state.

The synthesis result (host ID, prefix file path, content hash) is cached in the Session. As long as the host source file has not changed, subsequent compilations reuse the cached result without re-synthesizing.

## Multi-Context in Indexing

Compilation context affects not only live compilation but also index construction. The same header file may produce different symbols under different compilation contexts. For example:

```cpp
// config.h
#ifdef USE_V2
    using Handler = AsyncHandler;    // Visible in context A
#else
    using Handler = SyncHandler;     // Visible in context B
#endif
```

If the index only records the result from one context, go-to-definition or find-references would miss information from the other context. MergedIndex merges and stores index data produced from the same file under different compilation contexts, returning the union of all contexts on queries — for `config.h`, find-references can find references to both `AsyncHandler` and `SyncHandler`. See [Index Design](symbol-index.md) for the specific merging and deduplication mechanisms.

## FAQ

- **Can the same source file provide multiple contexts for the same header?**

  Yes. If the header has a header guard or `#pragma once`, multiple `#include` directives for it are effective only the first time, so a source file provides at most one context. But if the header has no include protection (e.g., X macro-style `.def` files), every `#include` introduces a new context. As described above, all effective include positions within a source file are numbered in order, so even when the same header is included multiple times, each inclusion can be uniquely distinguished by its number.

- **Is injecting the prefix file via `-include` fully equivalent to the original compilation?**

  For mechanisms defined by the C++ standard, yes. The only ways user code can observe file names and line numbers are `__FILE__`, `__LINE__`, and `std::source_location`. According to the C++ standard, [`#line` directives](https://en.cppreference.com/w/cpp/preprocessor/line) change the values of `__LINE__` and `__FILE__`, and [`std::source_location`](https://en.cppreference.com/w/cpp/utility/source_location) behavior is also determined by them. Therefore, the `#line` directives in the prefix file ensure that all three correctly reflect the target header's position information.

  However, two GCC/Clang compiler extensions behave differently: `__INCLUDE_LEVEL__` is 0 under `-include` mode (the target file is the main file), whereas in the original compilation it reflects the nested include depth; `__BASE_FILE__` returns the target header's path under `-include` mode, whereas in the original compilation it returns the host source file's path. These two extensions are rarely used in practice and have no impact on language server functionality.

- **Why write the prefix file to disk instead of using a virtual file?**

  Clang supports a virtual file system, so in theory the synthesized prefix code could exist only in memory without being written to disk. The choice to write disk files is mainly for debuggability: when users encounter problems, they can directly inspect the prefix file contents on disk, making it easier to diagnose issues and file bug reports. There is no particularly deep technical reason.

- **Why not use an approach that "compiles the host source file as the main unit and stops at the target position"?**

  Another approach that was considered: instead of synthesizing a prefix file, compile the host source file as the main compilation unit directly. Clang provides a declaration-level callback mechanism that allows checking the current source location after each top-level declaration is parsed. Using this mechanism, compilation can be stopped when parsing reaches the end of the target header based on the include position from the header context. At that point, the compiler has naturally accumulated the correct preprocessor state and can provide language services for the target header directly in that state.

  This approach has clear advantages: no files need to be synthesized, no include chains need to be tracked, and specifying the host source file alone solves the problem. Moreover, for non-self-contained headers like X macros that are embedded inside function bodies, since the compilation subject is the complete source file, the surrounding context (braces, statements, etc.) before and after the include position is intact, so there are no mismatched syntax issues.

  However, this approach has a critical flaw: **it cannot effectively leverage PCH optimization**. PCH works by precompiling and caching a contiguous preamble segment (typically a sequence of `#include` directives) at the beginning of a file. In the prefix synthesis approach, the target header is the compilation subject, and the synthesized prefix code is its preamble, which can be fully cached by PCH. But in the host compilation approach, the compilation subject is the host source file, and the target header is embedded somewhere in the include tree — typically not in the host source file's preamble region but somewhere in the middle of the file. Clang's PCH mechanism cannot split at such a position, so none of the content before the target header can be cached by PCH.

  For a header the user is actively editing, the file body is the most frequently updated part, requiring recompilation on each edit. Without PCH caching for the prefix content, every compilation would need to reprocess the large volume of preceding header inclusions in the host source file — an unacceptable cost in large projects. The prefix synthesis approach makes the target header the compilation subject and the prefix code a separately cacheable preamble, fully preserving PCH generation and usage.

## Known Limitations

- **Self-containedness detection is not yet automated**. In practice, the vast majority of headers are self-contained; only a few (X macro lists, internal implementation fragments, etc.) are not. Prefix code synthesis has non-trivial overhead — finding the host, computing the include chain, synthesizing prefix code, and pulling in a large number of preceding dependencies — so it should not be performed for every opened header. The ideal strategy is: treat headers as self-contained by default; if compilation produces a large number of specific error types (e.g., undefined symbols), automatically fall back to non-self-contained mode and recompile with synthesized prefix code. Existing index information (e.g., whether the header produced errors during compilation in other source files) could also help with this judgment. This automatic detection mechanism is not yet implemented.

- **Source file context switching is not fully implemented**. `clice/queryContext` can already list multiple CDB entries for a source file, but `clice/switchContext` currently only supports switching the host for headers and does not yet support switching between multiple compilation commands for the same source file. This is a known improvement item.

- **Determinism of host selection**. When multiple hosts are available, the current implementation selects the first source file found by BFS that has a CDB entry. A better strategy would prioritize the host "closest" to the target header — source files in the same directory, files with similar names, etc. — but this requires more refined heuristics.

- **No suffix handling**. The current prefix synthesis approach only injects content before the include position and does not handle content after the include position. For most headers this is not an issue, but there are two edge cases.

  First, non-self-contained headers like X macros that are embedded inside function bodies — the closing braces after the include position are lost:

  ```cpp
  // generate.cpp
  void register_all() {
  #define X(name, code, msg) register_error(code, msg);
  #include "errors.def"    // Prefix truncates here, the } below is lost
  #undef X
  }
  ```

  Second, when `#include` appears inside `#if`/`#ifndef` blocks, truncation produces unclosed conditional compilation directives:

  ```cpp
  // wrapper.h
  #ifndef USE_LEGACY
  #include <new_api.h>
  #include "target.h"      // Prefix truncates here, #ifndef has no matching #endif
  #else
  #include <old_api.h>
  #endif
  ```

  These incomplete syntax structures may cause Clang to report errors, but since the errors occur in the prefix file rather than the target header, they are typically filtered out. Suffix injection may need to be introduced in the future to handle these scenarios.

- **Recursive includes in prefix synthesis**. The include graph may contain cycles (broken by include guards or `#pragma once`). The dependency scanning stage already handles cycle detection correctly, but the prefix synthesis path has not been fully validated for this scenario.

- **PCH cache does not yet support cross-file sharing**. Currently, the PCH cache is keyed by the file's `path_id`, with each file maintaining its own PCH independently. If two different files happen to have identical preamble content, they each build their own PCH independently rather than sharing one. Changing the cache key to the preamble content hash to enable cross-file PCH sharing is a planned optimization.
