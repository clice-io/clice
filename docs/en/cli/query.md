# Query

## Overview

`clice query` asks the persisted index a structured question and prints the answer as JSON on stdout — for scripts and coding agents that want the facts a language server has without speaking LSP. It reads the index straight from disk, the same index `clice index` builds and an editor session keeps up to date, so it works while the editor is open and needs no server when it is not.

**Usage**: `clice query --method <question> [--workspace <dir>] [--configuration <tag>] [--fresh] [question options]`

## Questions

- `symbolSearch --query <text> [--limit <n>] [--kind <Kind,...>]` lists the symbols whose name contains the text, best matches first, with their kind, file, line and id.
- `definition`, `readSymbol`, `references [--include-declaration]`, `callGraph [--direction callers|callees|both]` and `typeHierarchy [--direction supertypes|subtypes|both]` answer about one symbol, named by `--name <name>` (optionally narrowed with `--path`), by `--symbol <id>` (the `#<hex>` id an earlier answer carried) or by `--path <file> --line <n>` (the symbol defined on that line). An ambiguous name lists the candidates' count and asks for an id.
- `documentSymbols --path <file>` is the file's outline.
- `compileCommand --path <file>` is the command the editor would compile the file with, and where it came from: the file's own database entry, a host source for a header, a rule's default command, a command inferred from a nearby unit, or the fallback.
- `projectFiles [--filter all|source|header|module]` lists the build's files; `fileDeps --path <file> [--direction includes|includers|both] [--depth <n>]` and `impactAnalysis --path <file>` follow the include graph.

Paths are workspace-relative or absolute in questions, absolute in answers; lines are 1-based.

## Answers

Every answer is one JSON object: `{"result": ..., "stale": [...]}` on success, `{"error": "...", "stale": [...]}` with exit code 1 when the question cannot be answered (an unknown symbol, a file that does not exist, an invalid option).

`stale` lists the files whose rows the answer had to leave out because their content on disk no longer matches what was indexed — the positions the index holds for them would point into text that moved. A file that changed only because a header it includes changed keeps answering from its last rows. A symbol whose definition sits in a stale file is reported as not found, with the file named, so a reader can tell "does not exist" from "not indexed yet". Only files the answer consulted are checked: a symbol added to a file after it was indexed is simply absent, since no row leads to that file — `--fresh` is the way to ask about the current disk. The index never sees unsaved editor buffers: the disk is the truth it describes.

`--fresh` brings the index up to date with the disk before answering: while an editor session has the workspace open, its server runs the sweep; otherwise the command runs the indexer itself, building the index from nothing when there is none yet. Either way only the units whose inputs changed are recompiled. Without `--fresh` a workspace with no index answers with an error.
