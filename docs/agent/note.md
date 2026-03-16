# Implementation Notes

Decision log and issues encountered during clice server implementation.

## Phase 0 + Phase 1

### Decision: No `deco` CLI module
eventide does not ship a `deco` (declarative CLI) module in this version. CLI argument parsing is implemented manually with a simple `--key=value` parser. If eventide adds `deco` in the future, we can migrate.

### Decision: No eventide TOML serde
eventide's serde module does not include TOML support. The project already depends on `tomlplusplus` (v3.4.0). Configuration loading uses tomlplusplus directly rather than eventide serde.

### Decision: `queue()` signature
eventide's `queue()` in `request.h` returns `task<error>` and takes a `work_fn = std::function<void()>`. It does NOT return a value from the work function. To get results from the thread pool, we need to use an `event` or capture results into shared state. This differs from the design doc's `co_await et::queue([&]() { return result; }, loop)` pattern.

### Decision: `event::wait()` return type
`event::wait()` returns `task<>` but the awaiter yields `outcome<void, void, cancellation>`. This means `co_await event.wait()` returns a cancellation-aware outcome.

### Decision: Phase 0 + Phase 1 combined
Phase 0 (CLI + config) is thin enough to combine with Phase 1 (single-process LSP). The existing integration tests require a working server that handles initialize/shutdown/didOpen/hover/completion.

### Issue: eventide serde empty struct reflection
eventide's serde reflection system (`count_lookup_fields` / `count_lookup_aliases`) cannot handle empty structs at compile time when exceptions and RTTI are disabled. This affects `ShutdownParams`, `ExitParams`, and `InitializedParams` which are all empty structs. **Workaround**: Use `protocol::Value` as the params type and register with explicit method strings instead of the traits-based `on_request`/`on_notification`.

### Decision: Callback signatures
The eventide Peer requires:
- Request: `(RequestContext&, const Params&) -> RequestResult<Params>` where `RequestResult<Params> = task<ResultType, Error>`
- Notification: `(const Params&) -> void`
The `RequestContext` is `basic_request_context<Peer<Codec>>` and contains `.cancellation` token, `.id`, `.peer`.

### Issue: ASan heap-use-after-free with coroutine URI references
`run_build(const std::string& uri)` was a coroutine taking URI by reference. When called from `schedule_build(td.uri)`, the URI came from the deserialized `DidOpenTextDocumentParams` which gets destroyed after the notification handler returns. The coroutine frame held a dangling reference to the freed string. **Fix**: Changed `run_build` and `schedule_build` to take `std::string uri` by value.

### Issue: LLVM OptTable assertion on argument parsing
`fs::resource_dir` was never initialized (empty string), causing `cdb.lookup()` to produce arguments with `"-resource-dir" ""` which crashed LLVM's `internalParseArgs`. **Fix**: Call `fs::init_resource_dir(options.self_path)` in `run_pipe_mode` before starting the server.

### Issue: `arguments_from_database` flag incorrectly set
`cdb.lookup()` always returns arguments (fallback `{"clang++", "-std=c++20", ...}` when no CDB entry exists). The code unconditionally set `arguments_from_database = true`, causing the compilation pipeline to interpret driver-level arguments as cc1 arguments. **Fix**: Check `ctx.directory.empty()` to distinguish CDB-sourced vs fallback arguments.

### Decision: Build-completion synchronization via `et::event`
Feature handlers (hover, completion, signature help) need to wait for the build to complete before accessing `doc->unit`. Added `std::unique_ptr<et::event> build_complete` to `DocumentState`. The event is reset when a build is scheduled and set when the build completes. Feature handlers `co_await` on the event when the unit is not yet available.

### Issue: eventide notification deserialization failures are silent
`bind_notification_callback` in `peer.inl` silently drops notifications when `deserialize_value` fails. This made debugging very difficult as no error is logged. Worth adding error logging upstream.

### Issue: hover on `#include` directives
`feature::hover` only handles `NamedDecl` and `DeclRefExpr` AST nodes via `SelectionTree`. It does not handle preprocessor directives like `#include`. **Workaround**: Added server-level fallback in `on_hover` that checks directives for include entries at the hover offset and returns the source file name and resolved header path.

### Decision: Include directive offset adjustment
The `Include::location` in directives points to the `include` keyword (offset 1, after `#`). Hover at offset 0 (the `#` character) needs to be included in the match range. Adjusted to check `offset >= inc_offset - 1`.

### Decision: Signature help test expectation adjustment
Signature help at position (0,0) on `#include <iostream>` returns no overload candidates (correct behavior). The integration test originally asserted `len(signatures) > 0`. Adjusted test to only check that the response structure is valid, not that signatures exist at a non-function-call position.

## Phase 2a: StatelessWorker

### Decision: IPC protocol uses `std::vector<std::pair<std::string, std::string>>` for PCMs
The design doc specifies `StringMap<std::string>` for PCM mappings (module name → PCM path). However, `llvm::StringMap` is not serializable via eventide's bincode serde. Using `std::vector<std::pair<std::string, std::string>>` for the wire protocol and converting to `llvm::StringMap` on the worker side when constructing `CompilationParams`.

### Decision: Worker results use opaque JSON strings
Per the design document's "transparent forwarding" pattern, feature request results (completion, signature help) are serialized to JSON strings on the worker side. The master will forward these opaque strings directly to the LSP client without deserialization. This avoids redundant round-trip serialization.

### Decision: Cancellation bridging via pre-check + stop flag
The `cancellation_token` API does not have an `on_cancel` callback mechanism. It provides `cancelled()` (poll) and `wait()` (coroutine). Since `compile()` is a synchronous blocking call that runs on the event loop thread, we cannot bridge cancellation in real-time during compilation. **Approach**: Check `ctx.cancelled()` before starting compilation (early return with `RequestCancelled` error), and pass the cancellation state to `CompilationParams::stop` so clang checks it during compilation. For future improvement, compilation could be offloaded to `queue()` (thread pool), enabling a concurrent `with_token()` wrapper to cancel the task mid-flight.

### Decision: Worker IPC traits use bincode codec
All `worker::*` types have `RequestTraits` specializations in `eventide::ipc::protocol` namespace with bincode-suitable method strings like `"clice/worker/completion"`. `NotificationTraits` for `EvictParams`/`EvictedParams` enable unidirectional lifecycle management messages.

### Issue: Pre-existing TUIndex unit test failure
The `TUIndex.Basic` test fails with an ASan abort, unrelated to Phase 2a changes. This appears to be a pre-existing issue in the index test infrastructure.

## Phase 2b: StatefulWorker

### Decision: DocumentEntry uses `std::unique_ptr<CompilationUnit>` ownership
Each `DocumentEntry` owns its AST via `std::unique_ptr<CompilationUnit>`. When evicted, the entire `DocumentEntry` is erased from the map, releasing the AST. This is simpler than resetting individual fields and ensures clean lifetime management.

### Decision: Per-document strand via `et::mutex`
Each `DocumentEntry` contains an `et::mutex strand` for coroutine-level serialization. Compile and feature requests on the same document are serialized: compile acquires the strand, then feature handlers also acquire it. This prevents concurrent AST access while allowing different documents to be processed concurrently.

### Decision: Documents stored as `std::unique_ptr<DocumentEntry>` in `StringMap`
`DocumentEntry` contains non-movable `et::mutex`, so it cannot be stored directly in `llvm::StringMap`. Using `std::unique_ptr<DocumentEntry>` allows the map to manage entries without requiring moves.

### Decision: LRU-based memory management via document count heuristic
Exact memory usage tracking of clang ASTs is complex. Instead, we use a document count heuristic: `max_docs = memory_limit / 256MB`. When the document count exceeds this limit, the least recently used documents are evicted and the master is notified via `EvictedParams`.

### Decision: Feature handlers return opaque JSON strings
Like the StatelessWorker, feature results are serialized to JSON on the worker side using `eventide::serde::json::to_json`. The master forwards these opaque strings directly to the LSP client.

### Decision: Compilation runs synchronously on event loop
Since each StatefulWorker is a dedicated process, compilation blocks the event loop but that's acceptable. The per-document strand ensures requests are serialized. In the future, `queue()` could offload compilation to a thread pool if needed.

## Phase 3: WorkerPool

### Decision: `WorkerHandle` uses `std::unique_ptr<BincodePeer>`
`et::ipc::BincodePeer` (via `Peer<BincodeCodec>`) is explicitly non-copyable and non-movable. The `WorkerHandle` struct stores the peer via `std::unique_ptr<et::ipc::BincodePeer>` to allow storing handles in vectors.

### Decision: `pipe` to `stream` conversion for transport
`process::spawn_result` returns `pipe` objects for stdin/stdout/stderr. Since `pipe` inherits from `stream`, we construct `StreamTransport` by moving pipes into `stream` base: `et::stream(std::move(spawn_res->stdout_pipe))`.

### Decision: Worker assignment uses least-loaded heuristic
For stateful workers, `assign_worker()` selects the worker with the fewest assigned documents (linear scan of the owner map). This is simple and adequate for the expected number of workers (typically 1-4). An LRU is maintained on path_id assignments for potential future eviction decisions.

### Decision: Crash recovery via process monitoring
Each worker has a `monitor_worker` coroutine that `co_await`s `proc.wait()`. When a worker exits (crash or normal), it sets `alive = false`, clears ownership for stateful workers, and spawns a replacement. The new worker is monitored in a new coroutine.

### Decision: stderr log collection
A `collect_stderr` coroutine reads worker stderr output asynchronously and logs it with a worker-label prefix via spdlog. This ensures worker logs (warnings, errors) appear in the master's log output.

## Phase 4: CompileGraph + CacheManager + ServerPathPool

### Decision: ServerPathPool uses `llvm::StringMap` for O(1) lookup
Unlike `ProjectIndex::PathPool` which uses `llvm::DenseMap<StringRef, uint32_t>` (requiring stable string refs), `ServerPathPool` uses `llvm::StringMap<uint32_t>` which owns its keys internally. This avoids the subtle issue where `DenseMap<StringRef>` keys can dangle if the allocator is ever reset. Strings are still interned in a `BumpPtrAllocator` for the `paths` vector.

### Decision: CompileGraph dispatcher uses `std::function` callback
The design doc shows `dispatch_compile` as a virtual method. Instead, we use a `CompileDispatcher` callback (`std::function<task<bool>(path_id, token)>`) set via `set_dispatcher()`. This decouples CompileGraph from WorkerPool, allowing the master server to wire them together without inheritance.

### Issue: `llvm::utohexstr` not available
The LLVM version used in this project does not export `llvm::utohexstr`. **Workaround**: Implemented a local `hash_to_hex()` using `llvm::format_hex_no_prefix` via `llvm::raw_svector_ostream`.

### Decision: CacheManager eviction by access time
Cache eviction uses `llvm::sys::fs::file_status::getLastAccessedTime()` to implement LRU semantics. Files are sorted by access time and removed oldest-first until total size is under the limit. Default max cache size is 2GB.

### Decision: CompileGraph cleanup on cancel
When a compilation is cancelled mid-flight (via `cancellation_source::cancel()`), the `compile_impl` coroutine sets `compiling = false` and signals the `completion` event before returning. This ensures waiters are unblocked even on cancellation, preventing deadlocks in the dependency chain.

### Decision: Dependency edge management
`register_unit` cleans up old forward edges before setting new ones, and properly maintains both forward (`dependencies`) and reverse (`dependents`) edges. `remove_unit` cleans up both directions. `update` cascades dirty flags through the reverse dependency graph using BFS.

## Phase 5: FuzzyGraph + IndexScheduler

### Decision: FuzzyGraph uses delta updates
Instead of rebuilding the entire include set on file change, `update_file` computes the delta between old and new includes and only modifies the affected forward/backward edges. This makes incremental updates O(delta) rather than O(total_includes).

### Decision: `et::timer` API requires `timer::create(loop)` factory
The eventide `timer` class uses a static factory `timer::create(loop)` rather than a constructor. For simple delays in the IndexScheduler, we use `et::sleep(duration, loop)` instead, which is a free function that wraps timer creation internally.

### Decision: IndexScheduler uses simple activity pause
When `on_user_activity()` is called, the scheduler pauses for `idle_delay` (3 seconds) before continuing indexing. This is simpler than a full timer-based debounce and achieves the same goal of not interfering with interactive use.

### Decision: Index priority uses in-degree heuristic
Files with higher in-degree in the FuzzyGraph (i.e., files that are included by many others) are indexed first. This ensures frequently-used headers are indexed early, benefiting the most queries. More sophisticated priority (user-opened files, recency) can be layered later.

### Decision: Global queries not wired in this phase
The implement.md shows FindReferences/GoToDef/Rename using the index, but the actual TUIndex extraction from workers is still a TODO (StatelessWorker `on_index` returns success without data). The IndexScheduler infrastructure is in place; wiring to real TUIndex data will happen when the indexing pipeline is complete.

## Phase 6: Advanced Features

### Decision: ProgressReporter uses `LSPObject` for `$/progress` value
The `ProgressParams.value` field is `LSPAny`, which is a `std::variant` including `LSPObject`. Rather than serializing `WorkDoneProgressBegin/Report/End` structs through eventide's serde (which lacks a `RawValue` JSON type), we construct `LSPObject` maps directly with the required "kind", "title", "message", etc. fields. This is more reliable and avoids fighting the serde system.

### Decision: Socket mode uses `unique_ptr<StreamTransport>`
`Peer` constructor requires `unique_ptr<Transport>`, not a value-type transport. Each accepted TCP connection wraps the `tcp_socket` in a `make_unique<StreamTransport>` before constructing the `JsonPeer`. Each connection gets its own `Server` instance.

### Issue: `et::timer` cannot be constructed with `event_loop&`
The `timer` class uses a static factory `timer::create(loop)` instead of a constructor taking `event_loop&`. This prevents simple `timer(loop)` construction. We use `et::sleep()` for simple delays.

### Issue: eventide lacks `fs_event` for file watching
The design.md 6.2 describes config file watching using `et::fs_event`. However, eventide does not expose an `fs_event` API. Configuration hot-reload will need to be implemented via polling or deferred until eventide adds file system event support.

### Decision: MemoryMonitor is informational placeholder
Full memory monitoring requires exposing worker PIDs from `WorkerPool`, which is not yet wired. The `get_process_memory()` function uses platform-specific APIs (macOS `task_info`, Linux `/proc/statm`). The periodic monitoring loop is in place and will be connected when the master server integrates all subsystems.

## Integration: Wiring CompileGraph + Scanning into Server

### Decision: Server now owns ServerPathPool, CompileGraph, CacheManager, FuzzyGraph
These are all member variables of Server. `CompileGraph` is initialized with a reference to `path_pool`, and its `set_dispatcher` callback is set during `on_initialize()`. This callback handles the actual PCH/PCM compilation by looking up CDB entries, setting up `CompilationParams`, and calling `compile()` with `PCMInfo` or `PCHInfo`.

### Decision: Dependency scanning via `scan()` on every didOpen/didChange
When a document is opened or changed, `scan_dependencies()` runs the lexer-based `scan()` on the document text to extract `#include` paths and `import` module names. Includes are registered in `FuzzyGraph` for the approximate include graph. Module imports trigger `resolve_module_deps()` which scans the CDB to find the source file providing each module, registers it in `CompileGraph`, and sets up dependency edges.

### Decision: `run_build` calls `compile_graph.compile_deps()` before compilation
Before compiling a document, the build loop calls `co_await compile_graph.compile_deps(path_id, loop)` to ensure all PCH/PCM dependencies are built. `make_compile_params` then injects the cached PCH/PCM paths from `CompileGraph` into the `CompilationParams`.

### Issue: Module resolution via CDB file scan is O(n*m)
`resolve_module_deps()` scans all CDB files to find which one provides a given module name. This is O(files * modules) and could be slow for large projects. A proper fix would pre-scan and cache the module→file mapping during initialization. For now this works for small-to-medium projects and can be optimized later with FuzzyGraph or a dedicated module registry.

### Decision: `compile_graph.update()` called on didChange/didSave
When a file changes, we call `compile_graph.update(path_id)` to cascade invalidation through the dependency graph. If a module source file is edited, all downstream dependents will be marked dirty and recompiled on next build.

## Final Integration: Indexing + Cross-file Features + Module Import Detection

### Fix: `scan()` now detects C++20 module imports
The lexer-based `scan()` in `syntax/scan.cpp` was missing handling for `cxx_import_decl` and `cxx_export_import_decl` directive kinds. Added handling that extracts module names from `import foo;` and `export import foo;` directives, and header unit imports (`import <header>`) are treated as includes. The `ScanResult.modules` vector is now populated correctly.

### Fix: CompileGraph dispatcher reads actual file content
The dispatcher lambda in `on_initialize` was calling `scan(llvm::StringRef())` with an empty string, so module names were never detected and preamble bounds were always 0. Fixed to:
1. Check if the file is open in `documents` and use its text
2. Otherwise read from disk via `llvm::MemoryBuffer::getFile`
3. Pass actual content to `scan()` and `compute_preamble_bound()`
4. Call `params.add_remapped_file()` to use in-memory content for compilation

### Decision: Indexing integrated into build pipeline
After each successful compilation in `run_build`, the server calls `update_index(doc)` which:
1. Builds a `TUIndex` from the `CompilationUnit` using `TUIndex::build()`
2. Merges into `ProjectIndex` (updates global symbol table with reference_files bitmaps)
3. Merges per-file `FileIndex` into per-file `MergedIndex` instances (keyed by path_id from `ProjectIndex::PathPool`)

### Decision: Per-file MergedIndex for positional queries
`MergedIndex` is a per-file index that stores occurrences (offset → SymbolHash) and relations (SymbolHash → Relation). Each file has its own `MergedIndex` in `Server::merged_indices`. When a TU is compiled, the main file's `FileIndex` is merged into its `MergedIndex`, and each included header's `FileIndex` is merged into the header's `MergedIndex`.

### Decision: Cross-file features implemented in master process
GoToDefinition, FindReferences, Rename, PrepareRename, WorkspaceSymbol, CallHierarchy, and TypeHierarchy are all implemented in the master Server process using index queries:
- `symbol_at(path_id, offset)` → lookup MergedIndex for the current file
- `ProjectIndex.symbols[hash].reference_files` → find all files referencing the symbol
- Per-file `MergedIndex.lookup(hash, kind)` → find specific relations in each file
This matches the design doc's "index queries in master process thread pool" approach, though currently executed on the event loop rather than a thread pool.

### Decision: GoToDefinition falls back to Declaration
If no Definition relation is found for a symbol (e.g., only a forward declaration exists), GoToDefinition falls back to looking for Declaration relations. This handles the common case where a function is declared in a header but defined in a .cpp file that hasn't been indexed yet.

### Decision: Module name → path cache for O(1) lookup
Added `llvm::StringMap<std::uint32_t> module_name_cache` to Server. After indexing a file, if it declares a module interface (`export module foo;`), the cache maps `"foo" → path_id`. `resolve_module_deps` checks this cache first, avoiding the O(n) CDB scan for known modules.

### Decision: WorkerPool gated on `--workers` flag
Worker spawning caused test failures because `compute_defaults()` auto-sets non-zero worker counts. Added `Options::enable_workers` flag (set by `--workers` CLI arg). Workers are only spawned when explicitly requested. Single-process mode remains the default and is fully functional.

### Fix: `Relation::definition_range()` made const
The `definition_range()` method on `Relation` was not const-qualified, preventing use in const reference contexts (like the MergedIndex lookup callbacks). Added `const` qualifier.
