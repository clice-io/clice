# Index

## Overview

`clice index` builds the persistent symbol index of a workspace ahead of time, so an editor session starts warm instead of indexing in the background while you work. It runs the same indexing pipeline as the server with a worker pool, and the two share one index per build configuration under `cache_dir`.

**Usage**: `clice index [--workspace <dir>] [--configuration <tag>] [--jobs <n>]`

One process writes the index of a workspace at a time. While an editor session has the workspace open, its server holds that role, so `clice index` asks the server to run the sweep and waits for the result instead of starting a worker pool of its own; the summary says so. A lock held by a process that cannot be asked (another `clice index`, a server of a different clice build, or one whose configuration keeps background indexing off) makes the command fail with the holder named — rerun it when that process is done.

The run is resumable: interrupting it with Ctrl-C or SIGTERM (what `timeout` and CI job limits send) saves the progress made so far, and the next invocation continues where it left off; only a second Ctrl-C quits at once without saving. Progress is also saved every five minutes, so a crash loses at most that much work. While it runs, a progress line goes to stderr every ten seconds whatever the log level; the final summary on stdout counts the indexed units, the file shards and symbols produced, lists every unit that failed to index, and names the session log directory. The exit code is 0 for a complete index, 1 when some units failed or part of the index could not be persisted, 2 when the arguments do not parse, and 130 when interrupted.

## Inspecting the index

The same command's operations read the persisted index without touching it. Each prints plain lines meant for a terminal or `grep`, and works while an editor session holds the index open.

- `clice index stats [--limit <n>]` prints what the index contains: the translation units and file shards, the byte split of the shard columns and of the global symbol table, how symbols distribute by reference-file count and name length, how file shards distribute by variant count, and the largest shards. `--variants` appends one tab-separated line per file shard with its variant count, for comparing two runs.
- `clice index symbol <locator>` prints what the index records about the symbols a [locator](./query.md#locators) names: their kind, name and flags, the parent chain that spells the qualified name, the file holding the canonical declaration, and per file how many definitions, declarations and references each has. A name names every symbol spelling it whole (`Box<int>`, `ns::Box<int>`) that `clice query` finds; an id names its symbol even when no file declares it, as with a macro defined on the command line.
- `clice index file <path>` prints a file's persisted rows: the content generation the rows were built from, every variant with the translation units that contributed it, the row counts by relation kind, and the file-local symbols whose identities live only in this file.
- `clice index unit <path>` prints a translation unit's manifest: when it was indexed, the files it contributed rows to, and its include tree.

## Examples

### Building the Index

A run indexes every translation unit of the build, except those a rule keeps out.

<!-- BEGIN GENERATED ITEMS: index/batch -->

<!-- BEGIN CAPABILITY: supported -->

**Build the index**

Index every translation unit of the build, so an editor session starts warm

```case
tests/index/batch/01_build_the_index
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Keep units out of indexing**

A rule with `index = false` keeps its units out; they still compile when opened

```case
tests/index/batch/02_keep_units_out
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

### Beside a Running Server

While an editor's server holds the index, `clice index` hands it the work.

<!-- BEGIN GENERATED ITEMS: index/delegation -->

<!-- BEGIN CAPABILITY: supported -->

**Through a running server**

While an editor's clice server runs, `clice index` and `--fresh` ask it to index instead of indexing alongside it

```case
tests/index/delegation/01_running_server
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
