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

1. **User selection takes priority**. If the user has actively selected a compilation context via `clice/switchContext`, that selection is used. Choices are persisted in the workspace cache and restored automatically when the file is opened in a later session.

2. **Direct CDB lookup**. If the file has an entry in the compilation database, that entry's compilation command is used. In multi-configuration projects where one file has several commands, the first one is used by default; the user can switch to another entry by command hash.

3. **Dependency graph lookup**. If the file is not in the CDB (the common case for headers), the dependency graph's include relationships are used to find a source file that includes it, and that source file's compilation command is borrowed. Host candidates are ranked by relevance: a source sharing the header's stem (`utils.h` -> `utils.cpp`) wins, then sources in the same directory, then path proximity, with a lexicographic tie-break for determinism. For self-contained headers, borrowing the compilation command is sufficient; for non-self-contained headers, prefix code must also be synthesized to restore the preprocessor state — the distinction is made automatically, see the next section.

4. **Command transfer heuristics (reserved, not yet implemented)**. For a file with neither a CDB entry nor any host, infer the best command from the CDB. Two strategies are planned: first, build a reverse table from the header search directories (`-I` etc.) of compile commands to the commands using them, and walk up from the file's parent directory — a header living under some command's search path borrows that command, and its own includes then resolve correctly (the typical case: a freshly created header not yet included by anything); second, nearest-path inference, borrowing from the CDB entry closest by path. This turns clangd's command interpolation into an explicit, observable tier instead of a hidden guess. The command source labeling already reserves an `Inferred` slot, and suspicious errors from inferred commands come with guidance diagnostics.

5. **Default command fallback**. When everything above falls through, an isolated file compiles with a synthesized default command, so basic language services keep working.

Beyond automatic resolution, clice also provides three LSP extension requests that let users explicitly query and switch compilation contexts:

**`clice/queryContext`** lists all available compilation contexts for a file. For headers, it returns the source files that can serve as hosts, ranked by relevance; for source files, it returns all CDB entries, using key flags from the compilation command (`-D`, `-std`, `-O`, etc.) as human-readable descriptions, each carrying a command hash identifying the entry. Results support pagination. For self-contained headers and source files, the context list is deduplicated by the **hash of the canonicalized compile arguments**: two contexts whose canonical arguments are identical produce identical compilation results, so only the best-ranked representative is kept — otherwise a widely-included header like `<vector>` could list thousands of contexts. Non-self-contained headers are not merged this way: different hosts synthesize different prefixes, so every host is a distinct context. When a guard-less header is included several times by one host, each include position is returned as a separate entry distinguished by an occurrence number.

**`clice/currentContext`** returns the currently active user selection (host + occurrence, or command hash). If the user has not actively switched (i.e., the automatically selected default context is in use), it returns empty.

**`clice/switchContext`** switches to the user's chosen new context. The choice is validated: the host must actually (transitively) include the header, the occurrence must be in range, and the command hash must correspond to a real CDB entry of the file, otherwise the request fails.

Besides the requests, after every compile the server pushes a **`clice/inactiveRegions`** notification carrying the file's preprocessor-inactive regions (bodies of untaken `#if` branches) under the current compilation context. Editors render them dimmed; a context switch recompiles and flips the regions — the most immediate visual feedback of switching.

## Automatic Self-Containedness Detection

The vast majority of headers are self-contained, and synthesizing a prefix for them is pure waste — it requires computing the include chain, reading every file on it, and pulling in a large number of preceding dependencies. clice therefore uses a **try-then-fall-back** strategy for headers without a CDB entry:

1. **Trial compile**: treat the header as self-contained and compile it directly with the borrowed host command (no prefix).
2. **Diagnostic scoring**: if the trial's diagnostics hit a strictly curated set of "missing context" signals — unknown type name, undeclared identifier, unterminated conditional directive — the header is judged non-self-contained. The set is deliberately narrow: a false positive merely costs one pointless prefix synthesis, while misclassifying ordinary in-progress typing errors would cause meaningless recompiles.
3. **Fallback recompile**: after the verdict, the header is recompiled with a synthesized prefix. The trial's error diagnostics are never published to the editor; the user only sees the final result.

Files with `.def` or `.inc` extensions (the X macro convention) skip the trial and are treated as non-self-contained directly. Only the "needs context" verdict is persisted in the workspace cache — it is the expensive side, and restarts should not repeat the trial; "self-contained" remains a session-local impression whose re-validation is free (the trial _is_ the regular compile). Re-evaluation timing is deliberately split: **dependency changes** (a chain file saved, a file the header includes changed, external on-disk modifications) re-run the trial, while **user typing** never does — otherwise a half-typed unknown type name would keep triggering pointless prefix synthesis. Saving the header itself also resets the persisted verdict.

## Header Context in Practice

The header context is conceptually clear (host source file + include position), but given a header context, how to actually make Clang compile the header under the correct preprocessor state is an engineering problem that requires choosing an approach.

> The prefix synthesis discussed here only applies to headers the user has opened. Headers on disk that are not open do not need special handling — they are included and processed normally when each source file is compiled. The indexing system collects symbol information from headers as part of each source file's indexing pass, and MergedIndex merges the index data produced for the same header under different source files (see [Index Design](symbol-index.md) for the merging mechanism).

clice uses **prefix synthesis + `-include` injection**: based on the host source file and include position from the header context, it extracts all content before the target header along the include chain, synthesizes it into prefix code, writes it to a prefix file on disk, and then injects that file into the compilation command via Clang's `-include` flag. The core advantage of this approach is its natural compatibility with PCH optimization — `-include`'d files are processed through Clang's predefines buffer before the main file, so when the preamble PCH is built, the prefix content is baked into the **same** PCH as the header's own preamble region; even a header with no directives of its own (an X macro style `.def` file) gets a PCH built for its prefix. On PCH reuse Clang validates and subsumes matching `-include`s, so the prefix is never processed twice. PCHs are cached by content key (preamble text + canonical flags), so files with identical prefixes automatically share one. When the user subsequently edits the header body, the large volume of header inclusions in the prefix does not need to be reprocessed each time. Detailed rationale for this design choice is in the FAQ section below. PCH construction, caching, and invalidation mechanisms are described in [Incremental Compilation Design](incremental-parse.md).

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

Several engineering details matter here. Include directives are resolved against the host command's real search paths and matched by **absolute path**, so same-named headers in different directories cannot be confused; since the prefix file lives in the cache directory, quoted includes with relative paths are rewritten to their resolved absolute paths, otherwise they would be looked up against the wrong base directory. Matching prefers includes outside `#if` blocks (an occurrence in an untaken branch must not shadow the real one); when the cut lands inside `#if` blocks (most commonly an include guard on an intermediate header), balancing `#endif`s are appended so the prefix stays well-formed — the guard condition is still evaluated by the compiler, preserving the semantics.

Beyond the prefix there is also a **suffix**: the content after the include position (mirrored along the chain — the direct includer's remainder first, the host's last) is synthesized into a suffix file and injected by appending a single `#include` line to the header's buffer at compile time. X macro fragments embedded in enums or function bodies thus see their surrounding braces close — the token stream runs continuously through prefix, main file and suffix. When the cut lands inside `#if` blocks, the prefix closes them with `#endif`s and the suffix reopens the same depth with `#if 1`s, keeping both sides balanced. Includes of the header itself along the chain (other occurrences) are redirected to a disk snapshot of it — the header's own path is remapped to the buffer with the trailing suffix include, so keeping them verbatim would recurse forever. The appended line sits past the editor's visible content, and diagnostics inside the suffix file are never attributed to the header itself.

The synthesis result (host, prefix file path, suffix file path, content hash, and the include chain with a content snapshot) is cached in the Session. The chain files' content is embedded in the prefix and the compiler never opens them, so regular dependency tracking is blind to them — staleness is handled by the chain snapshot (two-layer mtime + content hash detection): any change to a chain file re-synthesizes the prefix. Saving a chain file forces content re-validation even when its mtime is unchanged.

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

- **Occurrence numbering only covers the direct includer**. By design, a header context is uniquely determined by the host source file plus a position number in its include tree. The current implementation's occurrence only distinguishes multiple includes of the target in its **direct includer** (which covers the typical X macro usage), not a flattened numbering over the host's entire include tree — when the same header is transitively included by one host via different intermediate paths, only the context corresponding to the shortest chain is presented.

- **The shortest chain may not match the real first inclusion**. The include chain from host to target is the shortest path in the include graph. In a real compilation the header may first be reached via a different, longer path, and the preprocessor state accumulated before it may differ. In practice this rarely causes observable differences, but strictly speaking the synthesized state may not correspond exactly to any real compilation.

- **Cross-configuration conditional includes within one TU**. Dependency scanning records include edges per compilation configuration, but host lookup uses the union across configurations. In extreme cases, an include edge that only holds under configuration A could lend configuration B's command to a header.
