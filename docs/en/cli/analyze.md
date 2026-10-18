# Analyze

## Overview

`clice analyze` reads the persisted index and reports facts a coding agent turns into refactoring decisions. It never builds or times anything: it describes the code, and the agent — with the code at hand — decides what to change. Like `clice query`, it reads the index `clice index` builds and prints JSON on stdout.

**Usage**: `clice analyze <analysis> [<view> [<target>...]] [--workspace <dir>] [--configuration <tag>] [options]`

## Modules

`clice analyze modules` describes how the directories of a program depend on each other, as the input for partitioning it into C++20 named modules: which cycles stand in the way, which few entities each cycle hangs on, which headers belong elsewhere, and which constructs a mechanical rewrite into modules cannot carry over.

Every file in the scope belongs to a module, by default its directory. A module depends on another when one of its files names an entity a file of the other provides: a spelled name, a name a macro expansion produces in that file, or the template an explicit specialization specializes. The file providing an entity is a header defining it, else a header declaring it, else the source defining it. A dependency from a header is an interface edge, which would become an import of the module's interface; one only from sources is an implementation edge, which never closes a cycle because an implementation unit may import a module above it. Textual fragments such as `.inc` and `.def` files count as part of the files including them. A header only its own module's sources use, directly or through other such headers, is an internal partition: it adds nothing to the module's interface, and none of its dependencies are interface edges.

- `--scope <glob>` analyzes only the files the glob matches, as a workspace-relative path; repeat it for several. By default every indexed file under the workspace outside a dot directory.
- `--dir-depth <n>` names a file's default module by the first `n` segments of its directory instead of all of them.
- `--partition <file>` assigns modules by globs, first match wins: `{"modules": [{"name": "core", "files": ["src/support/**", "src/vfs/**"]}]}`. Files no glob claims keep their directory module. For the `interface` view and [modularize](./modularize.md), `"textual": true` keeps a module headers, `"external": true` marks one an existing module stands for (the standard library), and `"provides": "std.compat"` names the module exporting a module's names, read from the libc++ module sources `--std <dir>` names.
- `--move <path>=<module>` and `--merge <module>+<module>[+...]` evaluate a hypothetical change without making it; repeat them for several.
- `--move-entity <locator>=<header>` evaluates moving a declaration, with its members and what their definitions name, to another header, existing or new; repeat it for several moves. The [locator](./query.md#locators) names the declaration by its whole name: a function's overloads in one scope move together, the locator the `edge` view prints picks one of them, and a name several scopes spell is an error listing the candidates.
- `--annotation <file>` and `--churn-since <date>` weigh the results of the views that rank by them; see [annotations](#annotations).
- The view, a word after `modules`, chooses the answer, the overview when there is none.

| View                   | Answers                                                                                                                                                                                                                                                                                                                                                                                        |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| (no view)              | the modules by layer, the edges with sample entities, the cycles with the lightest set of edges whose removal breaks each, header cycles inside a module, the number of internal partitions, the deepest import chain, rebuild totals, hotspots, move and split candidates, and obstacle counts                                                                                                |
| `edge <from> <to>`     | every entity the edge carries, with its locator, owner and the `file:line` of each use                                                                                                                                                                                                                                                                                                         |
| `module <module>`      | each file's dependencies and dependents by module, the module's internal partitions, and its other headers grouped by the modules using them — the lines a directory splits along                                                                                                                                                                                                              |
| `file <path>`          | what the file names by module, who names its entities, and its annotations                                                                                                                                                                                                                                                                                                                     |
| `obstacles`            | every construct listed below                                                                                                                                                                                                                                                                                                                                                                   |
| `macros`               | macros used outside the file defining them, by module: imports carry no macros, so each reaches its users through a textual header                                                                                                                                                                                                                                                             |
| `impact`               | per header, the units an edit to it rebuilds today and under the partition, and whether it is an internal partition                                                                                                                                                                                                                                                                            |
| `interface [<module>]` | per module, what a wrapper module over its headers needs: the modules it imports, its entries with the name other modules include them by, its namespace-scope names with their headers, its namespace aliases, the headers that stay textual, the TU-local entities its headers share with other wrapped modules, the macros importers need, and the macros of other modules its headers read |

The overview's hotspots, move candidates and split candidates are capped by `--limit` (20 by default); modules, edges and cycles are always complete.

A move candidate is a header whose users all sit in one other module and that, together with the sources implementing it and the other headers those implement, names nothing of its own module and is named by nothing else there, short of the whole module; those files are listed to move along. A split candidate is a header whose entities fall into groups no consuming module shares, each listed as `name:line`, the first ten by line, with a `count` of all.

### Obstacles

- A header the index holds several variants of reads differently depending on what includes it; compiled once as a module interface it would keep one reading. `declarationsDiffer` marks variants declaring different entities, a real configuration dependence, and only those count in the overview; `unstable` names what only some variants name, such as the overloads a template's dependent call finds where it is included.
- A header testing a macro in a preprocessor condition without including its definition reads differently compiled alone, silently (`contextMacros`); one expanding such a macro fails to compile alone until it includes the definition (`borrowedMacros`).
- A `static` or anonymous-namespace entity of a header that another file uses, or that the header's own inline code uses, is local to one translation unit once the header is a module interface.
- A forward or friend declaration of an entity another module provides declares a different entity inside a module unit; one of a third-party entity attaches it to the wrong module. `unused` marks those the file never names, which can simply go.
- A declaration in one module's header whose definition sits in another module's source, and an entity several headers define.
- A macro a header defines that a third-party header reads (`configuringMacros`) has to stay ahead of that header's include in the global module fragment.
- A header specializing a third-party template for arguments it does not provide itself (`implicitProviders`) is used by every instantiation without being named, so no edge records those users; it stays where every instantiating unit can import it.
- Explicit specializations of a template from another module are listed too: they are legal, but reachable only where their module is imported.

### Rebuild estimates

Editing a module's interface recompiles every unit of every module importing it, directly or through other interfaces: a module's compiled interface records a hash of each one it imports. `impact` and the overview's totals compare that, counted in translation units, with the units an edit to the header rebuilds today. An edit to an internal partition rebuilds only the sources reaching it, and an edit to a source rebuilds one unit either way. The estimates are conservative and count only the program's sources.

## Annotations

An annotation is a named number per file that weighs the results, the way a profile weighs an optimizer's choices; every result stays meaningful without any.

- `churn` is how often a file changes: by default the commits of the last six months that touched it, from `git log` (`--churn-since <date>` takes any `git log --since` value; an empty value leaves it out). Hotspots and rebuild totals multiply by it.
- `compile_time` is what rebuilding a translation unit costs. Without it every unit counts as one.

An annotation file is `{"name": "compile_time", "unit": "s", "values": {"src/main.cpp": 4.2}}` with workspace-relative paths; one named like a computed annotation replaces it.

## Answers

Every answer is one JSON object, or `{"error": "...", "stale": [...]}` with exit code 1 when the question cannot be answered (no index, an unknown module or file, an invalid glob), or 2 when the arguments do not parse; an index missing some units fails with those units in `stale`.

## Examples

### Directory Modules

By default each directory is a module, and its dependencies follow what its files name.

<!-- BEGIN GENERATED ITEMS: analyze/dependencies -->

<!-- BEGIN CAPABILITY: supported -->

**Directory modules**

Each directory is a module, and a file naming another module's entity makes the dependency

A header naming another module's entity is an interface edge, which would
become an import of the module's interface; a source alone makes an
implementation edge. Outside a git repository the churn annotation is left
out, with a warning.

```case
tests/analyze/dependencies/01_directory_modules
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Cycles

Modules whose interfaces depend on each other form a cycle, listed with the edges to cut.

<!-- BEGIN GENERATED ITEMS: analyze/cycles -->

<!-- BEGIN CAPABILITY: supported -->

**Directory cycles**

Modules whose interfaces name each other form a cycle, listed with the lightest edges whose removal breaks it

Here util names two of core's entities and core one of util's, so the edge
from core to util is the one to cut. core's source calls into feature, and
feature's header names core's Logger: a source makes an implementation
edge, which closes no cycle.

```case
tests/analyze/cycles/01_directory_cycle
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Edges

The `edge` view lists what one module names of another.

<!-- BEGIN GENERATED ITEMS: analyze/views -->

<!-- BEGIN CAPABILITY: supported -->

**What an edge carries**

The edge view lists every entity one module names of another, with the locator that names it, the header that owns it and each use

An entity is an interface use when a header of the naming module names it,
as util/clock.h names both of these.

```case
tests/analyze/views/01_edge
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Weighing by Annotations

An annotation file weighs the results, here by compile time.

<!-- BEGIN GENERATED ITEMS: analyze/annotations -->

<!-- BEGIN CAPABILITY: supported -->

**Weighing by compile time**

An annotation file weighs each translation unit by what rebuilding it costs, here seconds of compile time

core/fast.h is included by app/main.cpp alone, but partitioned into modules
every source imports core, directly or through util's interface: an edit to
it rebuilds three units instead of one.

```case
tests/analyze/annotations/01_compile_time
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
