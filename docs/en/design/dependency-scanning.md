# Dependency Scanning

## Background

A language server needs the include relationships of the whole project from the start:

- **Headers need hosts.** A header has no entry in the compilation database; to compile it, clice needs a source that includes it — the reverse of the include graph.
- **Modules need a map.** Building a module needs to know which file declares each module name and what each file imports.

Exact include relationships take the full preprocessor — every macro expanded, every condition evaluated — and on tens of thousands of files that takes minutes. clangd builds its include graph as its background index compiles files, so until indexing finishes, which can take tens of minutes, a header opened may find no host and show false errors. clice runs a fast scanner of its own instead, and has a usable graph seconds after startup.

## Design

### A Superset, Shared by Every Includer

The scanner does not preprocess. Without macros or conditions evaluated, what a file includes no longer depends on who includes it, so each file is scanned once and its result shared by every includer: the work is the number of distinct files, not the number of (source, header) pairs. The price is that both branches of every `#if` count, giving a superset of the real include relationships. For what the graph is used for, a superset is safe: host lookup may find more candidates but never misses the real one — and each candidate is checked against its real compile before it hosts a header.

### Why It Is Fast

- **Directive scanning.** Clang's dependency directive scanner reads only the directive lines of a file, microseconds per file, and yields the raw include names (`"foo.h"`, `<vector>`).
- **Cached resolution.** Include names are resolved against cached directory listings rather than by a stat of each candidate path, and an angled include is resolved once per configuration and name.
- **Waves.** The scan starts with every translation unit of the build and the files their commands force in (`-include`, `-imacros`), resolves what they include, scans the headers newly found, and repeats until nothing new turns up. While one wave resolves, the next one's files are already being read.

### The Graph

- **Forward edges**: what each file includes. A unit is resolved under each of its commands, since search paths differ between them; a header under the first command that reaches it.
- **Reverse edges**, built in bulk, for climbing from a header to the units that include it.
- **Forced includes** kept apart from the include edges, as no directive in the source names them; host lookup climbs them too.
- **Module names** mapped to the units declaring them, interfaces and partitions. A module declaration the directive scanner cannot read — one inside an `#if` — makes the preprocessor run on that file alone, up to the declaration.

Each file's scan is kept against its content version and reused while the content holds. A changed file is rescanned and resolved as the startup scan reached it, which replaces its edges and scans the headers it newly reaches.

### Exact Answers Where Needed

Where an exact answer matters — the modules a unit imports, before each of its builds (see [Compile Task Graph](task-graph.md)) — the preprocessor runs on that unit. The fast scan says where that can be skipped: it records which files hold module syntax, and a unit reaching none of them through its includes imports nothing, since no macro can produce an `import`.

The background index compiles every unit with the full preprocessor and records the exact include trees in the index, which host lookup reads to check where a candidate's compile really enters a header. The fast scan's graph stays what finds the candidates.

## Design Decisions

- **Why a superset instead of exact results?** Exact results take minutes; the superset takes seconds and misses no host.
- **Why not wait for the background index?** Headers would be wrong for as long as indexing runs — tens of minutes on a big project. The fast scan gives a usable graph at once; the index refines it.
- **Why waves?** Headers are not in the database: they are found only by resolving what the sources include, and each wave's results name the next wave's files.

## Known Limitations

- **Macro includes** (`#include MACRO_NAME`) are not resolved by the scan; the compile handles them.
- **Both branches of every condition count**, so the graph is larger than any one compile's, and a file can look related to one it never includes.
- **A header is resolved under the first command reaching it.** A header two units reach through different include paths gets no host from the second until that unit is indexed.
- **The scan reads the disk.** An `#include` typed in an open buffer adds no edge until it is saved.
