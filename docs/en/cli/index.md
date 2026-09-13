# Index

## Overview

`clice index` builds the persistent symbol index of a workspace ahead of time, so an editor session starts warm instead of indexing in the background while you work. It runs the same indexing pipeline as the server with a worker pool, and the two share one index per build configuration under `cache_dir`.

**Usage**: `clice index [--workspace <dir>] [--configuration <tag>] [--workers <n>]`

The run is resumable: interrupting it with Ctrl-C saves the progress made so far, and the next invocation continues where it left off. Progress is also saved every five minutes, so a crash loses at most that much work. While it runs, a progress line goes to stderr every ten seconds whatever the log level; the final summary on stdout counts the indexed units, the file shards and symbols produced, lists every unit that failed to index, and names the session log directory. The exit code is 0 for a complete index, 1 when some units failed or part of the index could not be persisted, and 130 when interrupted.

## Inspecting the index

The same command reads the persisted index without touching it. Every mode prints plain lines meant for a terminal or `grep`, and works while an editor session holds the index open.

- `clice index --stats [--top <n>]` prints what the index contains: the translation units and file shards, the byte split of the shard columns and of the global symbol table, how symbols distribute by reference-file count and name length, how file shards distribute by variant count, and the largest shards. `--variants` appends one tab-separated line per file shard with its variant count, for comparing two runs.
- `clice index --show-symbol <name>` prints what the index records about a symbol: its kind, name and flags, the parent chain that spells its qualified name, the file holding its canonical declaration, and per file how many definitions, declarations and references it has. The name is matched as displayed (`Box<int>`) or qualified (`ns::Box<int>`); `#<hash>` names a symbol by its identity, which is how file-local symbols are reached.
- `clice index --show-file <path>` prints a file's persisted rows: the content generation the rows were built from, every variant with the translation units that contributed it, the row counts by relation kind, and the file-local symbols whose identities live only in this file.
- `clice index --show-tu <path>` prints a translation unit's manifest: when it was indexed, the files it contributed rows to, and its include tree.
