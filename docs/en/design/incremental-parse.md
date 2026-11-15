# Incremental Compilation

## Background

A few dozen lines of code expand, through their `#include`s, to tens or hundreds of thousands: `#include <vector>` alone is thousands of lines. Compiling all of it after every keystroke takes seconds. But the includes at the top of a file rarely change while the code below changes constantly:

```cpp
#include <vector>
#include <string>
#include "project/config.h"
// ── preamble ↑ changes slowly, its compile is cached ──
// ── code ↓ changes constantly, recompiled each time   ──

void process(const std::vector<std::string>& data) {
    // ...
}
```

So C++ language servers split a file into its **preamble**, the directives at the top, compiled once into a precompiled header (PCH), and the rest, recompiled over the PCH. clangd does too, with shortcomings in how it keeps and checks the PCH: it holds preambles in memory, which grows over long sessions ([clangd#251](https://github.com/clangd/clangd/issues/251)); its PCH files are temporary files a crash leaves behind ([clangd#209](https://github.com/clangd/clangd/issues/209)); a header touched without a change rebuilds the PCH; and nothing survives a restart.

clice keeps PCHs on disk, named by content; rebuilds one only when a dependency's content changed; compiles when a request needs the result, not on every edit; and never builds a PCH from a header name still being typed.

## Design

### The Preamble

The preamble runs from the top of the file to just before the first line that is not a directive. In a module unit it is the whole global module fragment, up to the module declaration. It ends early before a `#pragma` push it does not pop (`push_macro`, `pack(push)`, …), since a PCH keeps none of those stacks, and before a header name still being typed, such as an unclosed `#include "lib`: the finished directives above it stay precompiled, and the unfinished line parses with the body, where it reports its own error. A PCH built from a half-typed include would flood the file with false errors. In a global module fragment, a push followed by an include or code of the fragment's own leaves no preamble, and so does a header name being typed in a fragment that declares anything itself: the body would parse those ahead of the module declaration.

The boundary is found by a lexical scan, without a preprocessor.

### What the PCH Does for the Body

The PCH build also instantiates the function templates the preamble itself uses — the members of `std::vector<int>` a header's inline function calls — so the compile of the body finds them done, often most of the work an edit waits for.

Outside module units and the files importing a module, the PCH skips the bodies of the functions in the files the preamble includes, as clangd's does, keeping the `constexpr`, `consteval` and `auto` functions the compiler needs, and the templates forwarding their arguments (`make_unique`), whose bodies hold what features read through such a call, such as the parameter names an inlay hint shows. The body's compile then instantiates almost no header template's body, the bulk of an edit's work on template-heavy code, and holds a far smaller AST. What those bodies would report comes from the background index, which compiles every body: its run of the file brings back the diagnostics the editor's compile skipped, shown while the buffer holds the text it read. A rule's `skip_function_body` turns the skipping off, or extends it to every compile but the file's own, and to the build of the file's module or precompiled header.

### Is the PCH Still Good?

A PCH is stale when the content of a file it read changed. Comparing modification times alone rebuilds on every touch — a build tool's scan, a branch switch, an editor's autosave — and hashing every dependency on every check reads hundreds of headers. clice does both, in two layers, once, in the file table every freshness check shares (PCH and module validation, index staleness, disk polling):

1. **Stat.** The table keeps one observation per file: the size and mtime a read saw, and the hash of the bytes read. A stat equal to it means the hash is known without reading.
2. **Hash.** A file whose stat differs is read and hashed, which becomes its new observation. The PCH is stale only when the hash differs from the one of the bytes it was built from.

The bytes a build read are hashed from the compiler's own buffers, so a file changed during the build is never taken for what it read.

### Compiling When Asked

An edit only updates the buffer and marks its compile out of date; the compile runs when a request needs the result — a hover, completion, semantic tokens. A dozen keystrokes a second do not cost a dozen compiles. Changes on disk mark the open files depending on them out of date too, found by the server's polls, with the two-layer check before every compile as the backstop.

After a dependency changes — a saved header, a file a poll finds changed — open files do not wait for a request: a client that pulls diagnostics is asked to pull them again when it takes that request, and for one that does not pull, clice compiles the stale files and pushes theirs. Saves in a row compile an open file twice at most. The PCHs of files nobody has open are rebuilt when one is opened.

### Content-Addressed Storage

A PCH file is named by the hash of its preamble's text, the frontend-relevant flags, the directories and the clang version, so files whose preambles and commands agree share one file — unless the preamble defines or tests a macro, which places the PCH in the file that built it and adds that file's path to the name. Its record — the path, the hash, the boundary, what it read — is saved in the index database, so a restart checks each PCH and rebuilds only the stale ones. A changed preamble gets a new file; the old one is evicted when the cache outgrows its budget, coldest first.

A precompiled header the build declares — CMake's per target, or any command writing one that others read — is the one chain clice builds: a file reading it builds its preamble's PCH over it, so a change to its own preamble never parses the declared header's contents again. The declared PCH is built once and both the editor and the background index load it.

## FAQ

- **Why two layers instead of hashing alone?** Hashing every dependency on every check reads hundreds of headers. The stat layer narrows hashing to the files touched since the last look, usually none.
- **Why compile when asked instead of on every change?** clice keeps every file's PCH on disk, far more than clangd keeps in memory. A changed header can invalidate many of them; rebuilding them all at once would be an unacceptable volume, so only the PCH of the file someone asks about is rebuilt.
- **Is a PCH on disk slower than one in memory?** Clang maps the file and reads only the nodes a compile uses, so loading costs about the same either way; on disk it survives restarts, holds no process memory, and is shared between files.

## Known Limitations

- **A changed dependency rebuilds the whole PCH.** Only a precompiled header the build declares stays apart.
- **The completeness check covers header names alone.** Other half-typed directives, such as a `#define` whose value is being typed, build a PCH as they stand; the edit completing them rebuilds it.
- **No PCH for a global module fragment that imports.** A module unit whose fragment imports a module, usually through a header, parses its whole preamble on every edit: a PCH keeps no record that the fragment was open, and the module declaration after it would hide the modules imported there.
- **Templates the preamble instantiates see only the preamble.** The PCH instantiates the templates its headers use where the preamble ends, while a compiler does so at the end of the file, after the file's own declarations. An instantiation failing at the preamble's end is left to the file's end; one succeeding there differs from the compiler's in rare cases, whose errors the background index shows instead.
- **Errors inside headers' function bodies wait for the background index**, outside module units and the files importing a module, which parse every body. They show once it has compiled the file as saved, and an edit takes them down until the next save, as do the warnings about unused declarations. They never show where the background index does not compile the file as the editor does: with background indexing off, on a header compiled in its includer's context, on a file no database lists, or in a context the user chose. clang-tidy checks that read the bodies of called functions, such as `bugprone-exception-escape`, find less in the editor than `clice lint`.
