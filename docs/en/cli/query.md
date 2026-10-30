# Query

## Overview

`clice query` asks the persisted index a structured question and prints the answer as JSON on stdout — for scripts and coding agents that want the facts a language server has without speaking LSP. It reads the index straight from disk, the same index `clice index` builds and an editor session keeps up to date, so it works while the editor is open and needs no server when it is not.

**Usage**: `clice query <question> [<locator> | <file>] [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## Questions

- `symbol-search [<locator>] [--limit <n>]` lists the symbols a [locator](#locators) matches, best first, with their kind, file, line, container and `symbol`, and for a function the `signature` that tells its overloads apart.
- `definition <locator>`, `read-symbol <locator>`, `references <locator> [--include-declaration]`, `call-graph <locator> [--direction callers|callees|both]`, `type-hierarchy <locator> [--direction supertypes|subtypes|both]` and `implementation <locator>` answer about the one symbol the locator names: `definition` gives its declaration when nothing defines it, each reference names the function it lies in, and `implementation` lists the classes deriving from a class, or the overrides of a function, at every level below it. A locator several symbols match lists them as `candidates`; a name none spells exactly lists the closest matches the same way. `definition` and `read-symbol` also give the symbol's `documentation`: its documentation comment as markdown, read as the editor's hover reads it — `@param`, `@return` and `@copydoc` resolved, an undocumented override showing what it overrides.
- `document-symbols <file>` is the file's outline.
- `project-files [--filter all|source|header|module]` lists the build's files; `file-deps <file> [--direction includes|includers|both] [--depth <n>]` follow the include graph, and `impact-analysis <file>` the include graph and the files that import a module the file is part of.
- `counterparts <file>` lists the files the file pairs with — a header's sources, a source's headers, a module's interface and implementation units — best first, each with its reasons, and names the one an editor would open without asking as `preferred` (see [Switch Source/Header](../features/navigation.md#switch-source-header)).

Paths are relative to the current directory or absolute in questions, absolute in answers; lines are 1-based.

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
| `#1a2b3c`                    | the symbol with that id; one only some files see takes `path:` naming a file that holds it, relative or absolute                                                                                                                                           |
| `src/a.cpp:120`              | the symbols defined on that line                                                                                                                                                                                                                           |
| `src/a.cpp:120:8`            | the symbol under that cursor, with a 1-based line and byte column                                                                                                                                                                                          |
| `kind:function,method`       | only these kinds                                                                                                                                                                                                                                           |
| `path:src/index/`            | symbols declared in that file or under that directory, a path relative to the current directory; a bare file name, `path:a.cpp`, matches every file of that name                                                                                           |

A local or a parameter is known only to its file: a name finds it only together with `path:` naming the file or its directory. An entity of internal linkage at namespace scope — `static`, or in an anonymous namespace — a name finds wherever it is.

Every symbol an answer lists carries `symbol`, a locator naming it back: its id, followed by `path:` and its file for a symbol only some files see. Pass it back as it is. On a shell command line, quote the whole locator: `#` there starts a comment, `<` and `>` redirect and `*` expands.

## Answers

Every answer is one JSON object: `{"result": ..., "stale": [...]}` on success, `{"error": "...", "stale": [...]}` with exit code 1 when the question cannot be answered (an unknown symbol, a file that does not exist, an option value it does not take), or 2 when the arguments do not parse (an unknown question, a locator or file missing or too many). An error for a locator that matched several symbols lists them in `candidates`, each with its `symbol`.

`stale` lists the files whose entries the answer had to leave out because their content on disk no longer matches what was indexed — the positions the index holds for them would point into text that moved. A file that changed only because a header it includes changed keeps answering from its last index. A symbol whose definition sits in a stale file is reported as not found, with the file named, so a reader can tell "does not exist" from "not indexed yet". Only files the answer consulted are checked: a symbol added to a file after it was indexed is simply absent, since no entry leads to that file — `--fresh` is the way to ask about the current disk. The index never sees unsaved editor buffers: the disk is the truth it describes.

`--fresh` brings the index up to date with the disk before answering: while an editor session has the workspace open, its server runs the sweep; otherwise the command runs the indexer itself, building the index from nothing when there is none yet. Either way only the units whose inputs changed are recompiled. Without `--fresh` a workspace with no index answers with an error.

## Examples

### Questions

Each question names a symbol or a file and answers with what the index knows about it.

<!-- BEGIN GENERATED ITEMS: query/basics -->

<!-- BEGIN CAPABILITY: supported -->

**References of a symbol**

Every place naming a method, its declarations and the calls through the method it overrides included, each with the function it lies in

```case
tests/query/basics/01_references
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Search symbols by name**

A name query matches subsequences aligned to words

```case
tests/query/basics/02_symbol_search
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Questions about symbols**

Look a symbol up by name, by its id or by a line, and ask where it is defined and used, who calls it, what it derives from, what implements it and what a file declares

```case
tests/query/basics/03_symbol_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Questions about files**

List the project's files, what a file includes and what includes it, what a change to a header affects, and the source that goes with it

```case
tests/query/basics/04_file_questions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Documentation**

A symbol's documentation comment comes with its definition, read the way the editor shows it

```case
tests/query/basics/10_documentation
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**A symbol nothing defines**

The definition of a symbol nothing defines is its declaration, as go-to-definition answers in the editor

```case
tests/query/basics/11_declaration_only
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Answers through instantiations**

References and the call graph include what instantiations and macro expansions resolve, each marked with its `via`

A use the written code leaves open — a dependent call in a template, a name
in a macro body — answers with `"via": "instantiation"` or
`"via": "expansion"`; the written code's answers carry no `via`. A caller
carries one only when instantiations make all its calls.

```case
tests/query/basics/12_instantiations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Narrowing by Path

A `path:` term narrows a search to some files, and finds the names only one file can see.

<!-- BEGIN GENERATED ITEMS: query/names -->

<!-- BEGIN CAPABILITY: supported -->

**File-local symbols**

A static found by name anywhere; locals and parameters, which only their function names, found by name within their file with `path:`

```case
tests/query/names/05_file_local_names
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Narrow by path**

A `path:` filter keeps the symbols of the files under a path, spelled from the current directory, as a file name or absolute

```case
tests/query/names/06_path_filters
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Changed Files

Answers leave out the files changed since indexing, and `--fresh` brings the index up to date first.

<!-- BEGIN GENERATED ITEMS: query/freshness -->

<!-- BEGIN CAPABILITY: supported -->

**Files changed since indexing**

Answers leave out what the index holds for files changed on disk since it was built, and name those files in `stale`

```case
tests/query/freshness/01_changed_files_withheld
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Refresh before answering**

With `--fresh`, the index is brought up to date before the question is answered, and built if there is none yet

```case
tests/query/freshness/02_fresh_batch
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
