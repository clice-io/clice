# Compilation Context

## Background

`#include` is textual substitution: what a header means depends on the preprocessor state before the include. The same file can therefore compile to entirely different results — a source file under its Debug and its Release command, a header under each file that includes it.

A language server must pick one such context to serve a file, and every answer depends on that pick. Existing servers pick implicitly and give the user no way to see or change the pick; when it is wrong, the user sees a flood of false errors. In clangd this shows as long-standing issues: headers that are not self-contained do not work ([clangd#45](https://github.com/clangd/clangd/issues/45)), a header's command is guessed from file names and often wrong ([clangd#1505](https://github.com/clangd/clangd/issues/1505)), and of a file's several commands the user cannot choose one ([clangd#681](https://github.com/clangd/clangd/issues/681)).

clice makes the compilation context a first-class concept: every compile, index entry and answer knows which context it belongs to, and the user can list the contexts of a file and switch between them.

## What a Context Is

A compilation context is everything needed to restore a file's compilation state.

- For a **source file**, it is one of its commands. A file listed several times in the compilation database — Debug and Release, two platforms — has several contexts.
- For a **self-contained header**, one that includes everything it needs, it is the command of a source including it: the header compiles alone under that command's search paths and language standard.
- For a header that is **not self-contained**, such as an X macro list that only means something after its includer defines `X`, it is a **header context**: a host source file that includes it, and the position in the host's include tree where it is included. A header without an include guard can be included several times by one host, each time in another state, so the positions are numbered and a host plus a number names one context.

## Choosing a Context

A file compiles in the first of these that applies:

1. **The user's choice**, made through `clice/switchContext` and kept in the project's cache directory. A choice that stops holding — its host no longer includes the file, its entry left the database — waits, inactive, and applies again once it holds.
2. **The file's own entry** in a compilation database; of several, the first, until the user picks another.
3. **A host**: a source the build compiles that includes the header, ranked as [Command Resolution](command-resolve.md#rules-members-and-hosts) describes. A self-contained header takes its command; one that is not takes its command and a synthesized prefix (below).
4. **A rule's `default_command`.**
5. **A borrowed command**: a nearby unit's, for a file nothing describes yet — a new source, a header nothing includes (see [Command Resolution](command-resolve.md#rules-members-and-hosts)).
6. **clice's own command**, so an isolated file still gets basic answers.

The editor reaches this through clice's requests:

| Request                   | What it does                                                                                                                             |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `clice/queryContext`      | Lists a file's contexts: a source's entries, a header's hosts, best first and paged. Contexts that compile identically are listed once.  |
| `clice/currentContext`    | The context the file compiles in, whether the user chose it, and the command it compiles with and where that command comes from.         |
| `clice/switchContext`     | Picks one of the listed contexts; the file recompiles in it, without being closed.                                                       |
| `clice/resetContext`      | Drops the choice, back to the context clice picks.                                                                                       |
| `clice/showOtherContexts` | Whether the code the file's context skips shows what the file's other contexts compile there, from the index, which holds every context. |

Inside the chosen context, the branches the preprocessor skips are dimmed, so a switch shows at once.

## Build Configurations

A file's context is chosen per file; a **build configuration** is chosen per project. Rules can carry a `configuration` tag, the tags form a menu, and one is active in a server: its tagged rules apply, untagged rules always do. It is resolved once at startup from `--configuration`, the selection persisted from the editor, then `default_configuration` or the first tag.

Switching configurations restarts the server, on purpose: a server holds one configuration's whole world — its databases, dependency graph, contexts and index — and starting into another is the cold start every session performs anyway, warm from that configuration's own cache. Each configuration has an index database of its own, so switching never mixes two configurations' indexes, and switching back reindexes only what changed on disk meanwhile.

## Is a Header Self-Contained?

Most headers are, and synthesizing a prefix for them is wasted work. clice tries first: it compiles the header alone under its host's command. Without errors, the header is self-contained. With errors, it compiles the header once more with the host's prefix, and the compile with fewer errors stands: fewer in the host's context makes the header not self-contained, a tie keeps it self-contained. No list of what an includer provides is needed — whatever the context fixes counts. Fragment extensions by convention (`.def`, `.inc`, `.inl`, `.tpp`, …) skip the check.

The verdict is kept with the files the check read and holds while they do not change. A change of what the header includes runs the check again; typing in the header never does, or a half-typed name would keep switching it.

## Compiling a Header in Its Host's Context

clice compiles the header itself, with a synthesized **prefix** forced in through `-include`. Following the host's include tree down to the header, it takes from each file on the way everything before the include of the next one, each piece behind a `#line` so positions stay those of the original files:

```cpp
// main.cpp
#include <vector>
#define DEBUG 1
#include "utils.h"
```

```cpp
// utils.h
#pragma once
#include <string>
#include "math.h"
```

Opening `math.h` compiles it with this prefix forced in:

```cpp
#line 1 "main.cpp"
#include <vector>
#define DEBUG 1
#line 1 "utils.h"
#pragma once
#include <string>
```

The prefix exists only in memory, in pieces placed in the directories of the files they were cut from, so every relative lookup inside them resolves as it does in those files. A header inside braces its includer opens — an enumerator list, a class body — also gets a **suffix** that closes them. Any change to a file on the chain discards the context, and the next compile synthesizes it again.

The reason for `-include` is the preamble's PCH: what is forced in is compiled before the header's own text, so the whole prefix lands in the header's PCH, and an edit to the header recompiles only its body.

## Contexts in the Index

A header's entries differ from context to context. Its index shard keeps the entries of each context as a variant, and queries take the union of the live variants: references to both branches of a header's `#ifdef` are found. See [Symbol Index](symbol-index.md).

## FAQ

- **Is a header compiled through `-include` the same as in its host?** For everything the standard defines, yes: `#line` sets what `__FILE__`, `__LINE__` and `std::source_location` say. Two extensions differ: `__INCLUDE_LEVEL__` is 0, and `__BASE_FILE__` names the header rather than the host. Neither matters to the answers.

- **Why not compile the host and stop at the end of the header?** It needs no synthesis and keeps the surrounding braces intact, but the header then sits in the middle of the host's include tree, where no PCH can split. Every edit of the header would parse everything the host includes before it again, which is too slow on a large project. Making the header the compiled file keeps its prefix in a PCH.

## Known Limitations

- **A host is tried under its first configuration.** A header that only another command of a host enters is hosted elsewhere.
- **A chosen host that includes the header through a macro stops holding when it changes**, until a compile reads its include tree again; choosing the host again restores it.
- **Switching a build configuration restarts the server.**
