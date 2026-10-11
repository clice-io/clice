# Query

## Overview

`clice query` asks the persisted index a structured question and prints the answer as JSON on stdout — for scripts and coding agents that want the facts a language server has without speaking LSP. It reads the index straight from disk, the same index `clice index` builds and an editor session keeps up to date, so it works while the editor is open and needs no server when it is not.

**Usage**: `clice query --method <question> [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## Questions

- `symbolSearch --symbol <locator> [--limit <n>]` lists the symbols a [locator](#locators) matches, best first, with their kind, file, line, container and `symbol`.
- `definition`, `readSymbol`, `references [--include-declaration]`, `callGraph [--direction callers|callees|both]` and `typeHierarchy [--direction supertypes|subtypes|both]` answer about the one symbol `--symbol <locator>` names. A locator several symbols match lists them as `candidates`; a name none spells exactly lists the closest matches the same way.
- `documentSymbols --path <file>` is the file's outline.
- `compileCommand --path <file>` is the command the editor would compile the file with, and where it came from: the file's own database entry, a host source for a header, a rule's default command, a command borrowed from a nearby unit — for a provisional member (`provisional`) or a file outside the build (`inferred`) — or the fallback. The command is the one the compiler resolves it to, with its target and system include directories; when asking the compiler failed, `toolchainError` says why and the command stays the unresolved driver command.
- `projectFiles [--filter all|source|header|module]` lists the build's files; `fileDeps --path <file> [--direction includes|includers|both] [--depth <n>]` and `impactAnalysis --path <file>` follow the include graph.
- `counterparts --path <file>` lists the files the file pairs with — a header's sources, a source's headers, a module's interface and implementation units — best first, each with its reasons, and names the one an editor would open without asking as `preferred` (see [Switch Source/Header](../features/navigation.md#switch-source-header)).

Paths are workspace-relative or absolute in questions, absolute in answers; lines are 1-based.

## Locators

A locator names symbols, wherever a command takes one. It is one string: spaces separate its terms, quotes and angle brackets keep theirs. One term names the symbol, the others narrow the answer.

| Locator                      | Finds                                                                                                                                                                                                                                                      |
| ---------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `foo`                        | names matching `foo` as a subsequence aligned to their words — `LinLis` finds `LinkedList`, `up` finds `unique_ptr` — the exact name first, then the names starting with the query, then the rest; six letters or more also find names one typo away, last |
| `"foo"`                      | the whole name, case-sensitive                                                                                                                                                                                                                             |
| `foo*`, `*_test`, `get?Name` | names the glob matches, case-sensitive once the pattern has an uppercase letter                                                                                                                                                                            |
| `ns::Foo::bar`               | symbols inside a container whose chain lists `ns` and `Foo` in that order, other containers allowed around them                                                                                                                                            |
| `::ns::Foo::bar`             | symbols inside exactly that container                                                                                                                                                                                                                      |
| `ns::*`, `ns::**`            | the container's members; everything below it                                                                                                                                                                                                               |
| `Widget<int>`                | the specialization spelling those arguments                                                                                                                                                                                                                |
| `#1a2b3c`                    | the symbol with that id; one only some files see takes `path:` naming a file that holds it, workspace-relative or absolute                                                                                                                                 |
| `src/a.cpp:120`              | the symbols defined on that line                                                                                                                                                                                                                           |
| `src/a.cpp:120:8`            | the symbol under that cursor, with a 1-based line and byte column                                                                                                                                                                                          |
| `kind:function,method`       | only these kinds                                                                                                                                                                                                                                           |
| `path:src/index/`            | symbols declared in that file or under that directory, a relative path starting at the workspace root; a bare file name, `path:a.cpp`, matches every file of that name                                                                                     |

A local, a parameter and an entity of internal linkage — `static`, or in an anonymous namespace — are known only to the files that see them: a name finds them only together with `path:` naming their file or its directory.

Every symbol an answer lists carries `symbol`, a locator naming it back: its id, followed by `path:` and its file for a symbol only some files see. Pass it to `--symbol` as it is. On a shell command line, quote the whole locator: `#` there starts a comment, `<` and `>` redirect and `*` expands.

## Answers

Every answer is one JSON object: `{"result": ..., "stale": [...]}` on success, `{"error": "...", "stale": [...]}` with exit code 1 when the question cannot be answered (an unknown symbol, a file that does not exist, an option value it does not take), or 2 when the arguments do not parse. An error for a locator that matched several symbols lists them in `candidates`, each with its `symbol`.

`stale` lists the files whose rows the answer had to leave out because their content on disk no longer matches what was indexed — the positions the index holds for them would point into text that moved. A file that changed only because a header it includes changed keeps answering from its last rows. A symbol whose definition sits in a stale file is reported as not found, with the file named, so a reader can tell "does not exist" from "not indexed yet". Only files the answer consulted are checked: a symbol added to a file after it was indexed is simply absent, since no row leads to that file — `--fresh` is the way to ask about the current disk. The index never sees unsaved editor buffers: the disk is the truth it describes.

`--fresh` brings the index up to date with the disk before answering: while an editor session has the workspace open, its server runs the sweep; otherwise the command runs the indexer itself, building the index from nothing when there is none yet. Either way only the units whose inputs changed are recompiled. Without `--fresh` a workspace with no index answers with an error.
