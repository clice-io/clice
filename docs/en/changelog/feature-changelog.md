# Feature Changelog

This file tracks user-facing feature changes across clice releases. Each release section documents new features, breaking changes, and deprecations.

<!-- Add release entries below in reverse chronological order. -->
<!-- Format:
## vX.Y.Z (YYYY-MM-DD)

### Added
- ...

### Changed
- ...

### Fixed
- ...

### Removed
- ...
-->

## v1.0.0 (upcoming)

v1.0 ships first as rolling beta releases (`v1.0.0-beta.1`, `beta.2`, …) for a public trial period; fixes land continuously in small versions with no code freeze, and the final `v1.0.0` is tagged once the trial settles. This entry summarizes everything user-visible since the `v0.1.0-alpha` series.

### Added

- **Multi-process architecture.** Compilation runs in worker subprocesses coordinated by a master process, so a compiler crash on one translation unit no longer takes down the whole session — the crashed worker is respawned and its documents recover on demand. Stateful workers cap resident documents with LRU eviction, and the pool sheds background load under system memory pressure.
- **Compilation contexts for headers.** Opening a header compiles it in the context of a source file that includes it: clice reconstructs the include chain into a synthesized preamble (and a synthesized suffix for X-macro/`.def` fragments), so symbols that depend on the includer's macros resolve correctly. Editors can list and switch the active context through the `clice/queryContext`, `clice/currentContext`, and `clice/switchContext` extensions — the same mechanism switches between multiple compile-database entries for one file. Self-contained headers are detected and compiled directly.
- **LSP feature surface.** Hover (definition printing, documentation, record layout with size/offset/padding, expression values, `auto`/`decltype` deduction); code completion with signature details, parameter snippets, deprecation strikethrough, and `#include`/`import` path completion; signature help; go-to definition (including on `#include` lines and module names), declaration, implementation, type definition, and find references, served from the project index; document symbols; semantic tokens with rich modifiers; inlay hints; folding ranges; document links (including inside the preamble, and `#embed`); `.clang-format`-based formatting and range formatting; call hierarchy and type hierarchy; workspace symbol search; published diagnostics; inactive-region greying for code disabled by `#if` (`clice/inactiveRegions` extension).
- **Background indexing.** A persistent project-wide index powers cross-file features without opening every file. Indexing piggybacks on compilations that already happen, runs concurrently at low priority, pauses for interactive requests, reports LSP `$/progress`, and survives restarts. Two-layer staleness detection (mtime, then content hash) keeps `touch` and `git checkout` from triggering needless rebuilds.
- **Out-of-band change tracking.** A stat-polling file tracker picks up changes made outside the editor — a regenerated `compile_commands.json`, a `git checkout`, code generators — without restarting the server.
- **C++20 named modules.** Module interfaces are compiled on demand through a pull-based dependency graph with PCM caching; `import` completion and go-to-definition on module names included.
- **Unified on-disk cache.** PCH, PCM, and index artifacts live in one content-addressed store with crash-safe writes, shared across sessions and restarts; the rebuildable artifacts (PCH/PCM) are evicted under a size bound.
- **Configuration.** `clice.toml` (or `.clice/config.toml`) plus LSP `initializationOptions` overlay; per-file `[[rules]]` with glob patterns to append/remove compile flags; `[tracker]` polling intervals; XDG-based cache paths with `${workspace}` substitution. Malformed configuration is reported with line/column as diagnostics on the config file.
- **Tooling API.** A JSON-RPC API over TCP for AI agents and external tools: project files, symbol search, reading full symbol bodies, definitions/references, call graph, and type hierarchy — plus the `clice query` CLI.
- **Editor extensions.** In-repo extensions for VS Code (published on the Marketplace), Neovim, and Zed.
- **Operability.** Per-session file logging with crash capture, user-facing guidance via `window/logMessage` when the setup is broken (e.g. no compilation database), LSP session recording for replay (`--record`), and an accurate `--version` that matches the release tag.

### Changed

- **Configuration file location.** The `config` CLI option is gone; configuration is read from `${workspace}/clice.toml` (or `${workspace}/.clice/config.toml`).
- **Config key rename.** `compile_commands_dirs` is now `compile_commands_paths`; the old key is ignored.
- **Upgrading from 0.x rebuilds caches.** The on-disk cache and index formats changed; the first launch after upgrading discards old artifacts and re-indexes the project once. This also retires PCHs produced by older versions whose cache key ignored compile flags — those could serve results with the wrong macro configuration.
- **Toolchain baseline.** clice now builds against LLVM/Clang 21.1.8, with prebuilt release binaries for Linux, macOS, and Windows on both x64 and arm64.

### Fixed

- Rapid consecutive edits no longer produce spurious "redefinition" errors from a compile/update race.
- Several worker crashes on malformed input (null AST consumer, invalid file IDs, missing cache directories) fixed; crashes that do happen are contained by process isolation.
- Documents evicted from a worker's LRU no longer silently lose hover/semantic tokens — they recompile on the next request.
- Lifecycle edge cases: a `didOpen` racing ahead of the `initialize` handshake is accepted; unknown enum values from newer LSP clients no longer fail the handshake; diagnostics produced before the client was ready are replayed after `initialized`.
- Saving a header now correctly reindexes closed files that include it, keeping cross-file references fresh.

### Known gaps

- Code completion, navigation, document links, and diagnostics are functional but not yet complete (see the feature overview pages for per-feature status).
- Code actions are not implemented yet (and not advertised).
- clang-tidy integration is planned; the `clang_tidy` config option is parsed but has no effect.
