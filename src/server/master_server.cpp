#include "server/master_server.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "command/search_config.h"
#include "eventide/ipc/json_codec.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/reflection/enum.h"
#include "eventide/serde/json/json.h"
#include "eventide/serde/serde/raw_value.h"
#include "index/tu_index.h"
#include "semantic/symbol_kind.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/dependency_graph.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
namespace lsp = eventide::ipc::lsp;
namespace refl = eventide::refl;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;


MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop),
    peer(peer),
    pool(loop),
    indexer(path_pool),
    compiler(path_pool, pool, indexer, config, cdb, dependency_graph),
    self_path(std::move(self_path)) {}

MasterServer::~MasterServer() = default;

std::string MasterServer::uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
}

void MasterServer::publish_diagnostics(const std::string& uri,
                                       int version,
                                       const et::serde::RawValue& diagnostics_json) {
    std::vector<protocol::Diagnostic> diagnostics;
    if(!diagnostics_json.empty()) {
        auto status = et::serde::json::from_json(diagnostics_json.data, diagnostics);
        if(!status) {
            LOG_WARN("Failed to deserialize diagnostics JSON for {}", uri);
        }
    }
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.version = version;
    params.diagnostics = std::move(diagnostics);
    peer.send_notification(params);
}

void MasterServer::clear_diagnostics(const std::string& uri) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = {};
    peer.send_notification(params);
}


et::task<> MasterServer::load_workspace() {
    if(workspace_root.empty())
        co_return;

    // Create cache directory if configured
    if(!config.cache_dir.empty()) {
        auto ec = llvm::sys::fs::create_directories(config.cache_dir);
        if(ec) {
            LOG_WARN("Failed to create cache directory {}: {}", config.cache_dir, ec.message());
        } else {
            LOG_INFO("Cache directory: {}", config.cache_dir);
        }

        for(auto* subdir: {"cache/pch", "cache/pcm"}) {
            auto dir = path::join(config.cache_dir, subdir);
            auto ec2 = llvm::sys::fs::create_directories(dir);
            if(ec2) {
                LOG_WARN("Failed to create {}: {}", dir, ec2.message());
            }
        }

        compiler.cleanup_cache();
        compiler.load_cache();
    }

    // Search for compile_commands.json
    std::string cdb_path;

    if(!config.compile_commands_path.empty()) {
        if(llvm::sys::fs::exists(config.compile_commands_path)) {
            cdb_path = config.compile_commands_path;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}",
                     config.compile_commands_path);
        }
    }

    if(cdb_path.empty()) {
        for(auto* subdir: {"build", "cmake-build-debug", "cmake-build-release", "out", "."}) {
            auto candidate = path::join(workspace_root, subdir, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                cdb_path = std::move(candidate);
                break;
            }
        }
    }

    if(cdb_path.empty()) {
        LOG_WARN("No compile_commands.json found in workspace {}", workspace_root);
        co_return;
    }

    auto count = cdb.load(cdb_path);
    LOG_INFO("Loaded CDB from {} with {} entries", cdb_path, count);

    auto report = scan_dependency_graph(cdb, path_pool, dependency_graph);
    dependency_graph.build_reverse_map();

    auto unresolved = report.includes_found - report.includes_resolved;
    double accuracy =
        report.includes_found > 0
            ? 100.0 * static_cast<double>(report.includes_resolved) / report.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), "
        "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        report.elapsed_ms, report.total_files, report.source_files, report.header_files,
        report.total_edges, report.includes_resolved, report.includes_found, accuracy,
        report.waves);
    if(unresolved > 0) {
        LOG_WARN("{} unresolved includes", unresolved);
    }

    compiler.build_module_map();

    // Load persisted index from disk.
    indexer.load(config.index_dir);

    if(config.enable_indexing) {
        for(auto& entry: cdb.get_entries()) {
            auto file = cdb.resolve_path(entry.file);
            auto server_id = path_pool.intern(file);
            index_queue.push_back(server_id);
        }
        if(!index_queue.empty()) {
            LOG_INFO("Queued {} files for background indexing", index_queue.size());
            schedule_indexing();
        }
    }

    compiler.init_compile_graph();
}

/// Pull-based compilation entry point for user-opened files.
///
/// Called lazily by forward_stateful() / forward_stateless() before every
/// feature request (hover, semantic tokens, etc.). Guarantees that when it
/// returns true the stateful worker assigned to `path_id` holds an up-to-date
/// AST and diagnostics have been published to the client.
///
/// Lifecycle overview (pull-based model):
///
///   didOpen / didChange          – only update DocumentState, mark ast_dirty
///   didSave                      – mark dependents dirty, queue indexing
///   feature request arrives      – calls ensure_compiled() first
///     1. Fast-path exit if AST is already clean (!ast_dirty).
///     2. Compile any C++20 module dependencies (PCMs) via CompileGraph.
///     3. Build / reuse the precompiled header (PCH) via ensure_pch().
///     4. Send CompileParams to the stateful worker, which builds the AST.
///     5. On success: publish diagnostics, clear ast_dirty, schedule indexing.
///     6. On generation mismatch (user edited during compile): keep dirty,
///        the next feature request will trigger another compile cycle.
///
/// Only the opened file itself is remapped (its in-memory text is sent to the
/// worker); every other file is read from disk by the compiler.
///
/// Concurrency: multiple concurrent feature requests for the same file will
/// each call ensure_compiled(). The first one launches a detached compile
/// task via loop.schedule(); subsequent ones wait on the shared event.
/// The detached task cannot be cancelled by LSP $/cancelRequest, preventing
/// the race where cancellation wakes all waiters and they all start compiles.
et::task<bool> MasterServer::ensure_compiled(std::uint32_t path_id) {
    auto it = documents.find(path_id);
    if(it == documents.end()) {
        LOG_WARN("ensure_compiled: doc not found for path_id={} path={}",
                 path_id,
                 path_pool.resolve(path_id));
        co_return false;
    }

    auto& doc = it->second;
    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} ast_dirty={}",
              path_id,
              doc.version,
              doc.generation,
              doc.ast_dirty);

    if(!doc.ast_dirty) {
        if(!compiler.is_stale(path_id)) {
            co_return true;
        }
        doc.ast_dirty = true;
    }

    // If another compile is already in flight, wait for it.
    // This co_await may be cancelled by LSP $/cancelRequest — that's fine,
    // it just means this particular feature request is abandoned.  The
    // detached compile task keeps running independently.
    while(it->second.compiling) {
        auto pending = it->second.compiling;
        co_await pending->done.wait();
        it = documents.find(path_id);
        if(it == documents.end())
            co_return false;
        if(!it->second.ast_dirty)
            co_return true;
    }

    // No compile in flight and AST is dirty — launch a detached compile task.
    // The detached task is scheduled via loop.schedule() so it is NOT subject
    // to LSP $/cancelRequest cancellation.  This eliminates the race where
    // cancellation fires the RAII guard, waking all waiters simultaneously
    // and causing them all to start new compiles.
    auto pending_compile = std::make_shared<DocumentState::PendingCompile>();
    it->second.compiling = pending_compile;

    LOG_INFO("ensure_compiled: launching detached compile path_id={} gen={}",
             path_id,
             doc.generation);

    loop.schedule([](MasterServer* self,
                     std::uint32_t pid,
                     std::shared_ptr<DocumentState::PendingCompile> pc) -> et::task<> {
        // All parameters are copied into the coroutine frame as function args,
        // so they survive the lambda temporary's destruction.
        auto finish_compile = [&]() {
            if(auto it = self->documents.find(pid); it != self->documents.end()) {
                if(it->second.compiling == pc) {
                    it->second.compiling.reset();
                }
            }
            LOG_INFO("ensure_compiled: finish_compile (detached) path_id={}", pid);
            pc->done.set();
        };

        auto it = self->documents.find(pid);
        if(it == self->documents.end()) {
            finish_compile();
            co_return;
        }

        auto gen = it->second.generation;
        LOG_INFO("ensure_compiled: starting compile (detached) path_id={} gen={}", pid, gen);

        auto file_path = std::string(self->path_pool.resolve(pid));
        auto uri = lsp::URI::from_file_path(file_path);
        std::string uri_str = uri.has_value() ? uri->str() : file_path;

        // ── Phase 1–3: Module deps, PCH, PCM paths ─────────────────────
        worker::CompileParams params;
        params.path = file_path;
        params.version = it->second.version;
        params.text = it->second.text;
        if(!self->compiler.fill_compile_args(self->path_pool.resolve(pid),
                                            params.directory,
                                            params.arguments)) {
            finish_compile();
            co_return;
        }

        if(!co_await self->compiler.ensure_deps(pid,
                                       params.path,
                                       params.text,
                                       params.directory,
                                       params.arguments,
                                       params.pch,
                                       params.pcms)) {
            LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
            finish_compile();
            co_return;
        }

        it = self->documents.find(pid);
        if(it == self->documents.end()) {
            finish_compile();
            co_return;
        }

        // ── Phase 4: Dispatch to stateful worker ────────────────────────
        auto result = co_await self->pool.send_stateful(pid, params);

        // Re-lookup: the document may have been closed while we were compiling.
        it = self->documents.find(pid);
        if(it == self->documents.end()) {
            finish_compile();
            co_return;
        }

        auto& doc2 = it->second;

        // ── Phase 5: Handle result ──────────────────────────────────────
        if(doc2.generation != gen) {
            LOG_INFO("ensure_compiled: generation mismatch ({} vs {}) for {}",
                     doc2.generation,
                     gen,
                     uri_str);
            finish_compile();
            co_return;
        }

        if(!result.has_value()) {
            LOG_WARN("Compile failed for {}: {}", uri_str, result.error().message);
            self->clear_diagnostics(uri_str);
            finish_compile();
            co_return;
        }

        doc2.ast_dirty = false;
        pc->succeeded = true;
        self->compiler.record_deps(pid, result.value().deps);

        // Store open file index from the stateful worker's TUIndex.
        if(!result.value().tu_index_data.empty()) {
            auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
            OpenFileIndex ofi;
            ofi.file_index = std::move(tu_index.main_file_index);
            ofi.symbols = std::move(tu_index.symbols);
            ofi.content = doc2.text;
            self->indexer.set_open_file(pid, file_path, std::move(ofi));
        }

        finish_compile();

        // Publish diagnostics AFTER marking compile as done, so that concurrent
        // forward_stateful() calls can proceed immediately.
        self->publish_diagnostics(uri_str, doc2.version, result.value().diagnostics);
        self->schedule_indexing();
    }(this, path_id, pending_compile));

    // Wait for the detached compile to finish.  If this wait is cancelled
    // by LSP $/cancelRequest, the detached task continues unaffected.
    co_await pending_compile->done.wait();

    it = documents.find(path_id);
    if(it == documents.end())
        co_return false;

    co_return !it->second.ast_dirty;
}

// =========================================================================
// Index integration (scheduling — data/queries are in Indexer)
// =========================================================================

void MasterServer::schedule_indexing() {
    if(!config.enable_indexing || indexing_active || indexing_scheduled)
        return;
    indexing_scheduled = true;

    // Create or reset idle timer.
    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<et::timer>(et::timer::create(loop));
    }
    index_idle_timer->start(std::chrono::milliseconds(config.idle_timeout_ms));
    loop.schedule(run_background_indexing());
}

et::task<> MasterServer::run_background_indexing() {
    // Wait for idle timeout before starting.
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    std::size_t processed = 0;

    while(index_queue_pos < index_queue.size()) {
        auto server_path_id = index_queue[index_queue_pos];
        index_queue_pos++;

        auto file_path = std::string(path_pool.resolve(server_path_id));

        // Skip open files — their index comes from the stateful worker and is
        // stored in open_file_indices.  When closed, they rejoin the queue.
        if(documents.count(server_path_id)) {
            continue;
        }

        if(!indexer.need_update(file_path))
            continue;

        // Prepare IndexParams for the stateless worker.
        worker::IndexParams params;
        params.file = file_path;
        if(!compiler.fill_compile_args(file_path, params.directory, params.arguments))
            continue;

        // Fill PCM deps for module-aware indexing.
        compiler.fill_pcm_deps(params.pcms);

        LOG_INFO("Background indexing: {}", file_path);

        auto result = co_await pool.send_stateless(params);
        if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
            LOG_INFO("Background indexing got TUIndex for {}: {} bytes",
                     file_path,
                     result.value().tu_index_data.size());
            indexer.merge(result.value().tu_index_data.data(),
                          result.value().tu_index_data.size());
            ++processed;
        } else if(result.has_value() && !result.value().success) {
            LOG_WARN("Background index failed for {}: {}", file_path, result.value().error);
        } else if(result.has_value() && result.value().tu_index_data.empty()) {
            LOG_WARN("Background index returned empty TUIndex for {}", file_path);
        } else {
            LOG_WARN("Background index IPC error for {}: {}", file_path, result.error().message);
        }
    }

    indexing_active = false;
    LOG_INFO("Background indexing complete: {} files processed", processed);

    // Persist index to disk after a full pass.
    indexer.save(config.index_dir);
}

// =========================================================================
// Include/import completion (handled in master)
// =========================================================================

PreambleCompletionContext MasterServer::detect_completion_context(const std::string& text,
                                                                  uint32_t offset) {
    // Find the start of the line containing offset.
    auto line_start = text.rfind('\n', offset > 0 ? offset - 1 : 0);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;

    // Find the end of the line.
    auto line_end = text.find('\n', offset);
    if(line_end == std::string::npos)
        line_end = text.size();

    // Extract the line up to the cursor position.
    auto line = llvm::StringRef(text).slice(line_start, offset);

    // Strip leading whitespace.
    auto trimmed = line.ltrim();

    // Check for #include "prefix or #include <prefix
    if(trimmed.starts_with("#")) {
        auto directive = trimmed.drop_front(1).ltrim();
        if(directive.consume_front("include")) {
            directive = directive.ltrim();
            if(directive.consume_front("\"")) {
                return {CompletionContext::IncludeQuoted, directive.str()};
            }
            if(directive.consume_front("<")) {
                return {CompletionContext::IncludeAngled, directive.str()};
            }
        }
        // Line starts with # but isn't #include — not a completion context.
        return {};
    }

    // Check for [export] import prefix (without trailing semicolon).
    auto import_check = trimmed;
    if(import_check.consume_front("export") && !import_check.empty() &&
       !std::isalnum(import_check[0])) {
        import_check = import_check.ltrim();
    }
    if(import_check.consume_front("import") &&
       (import_check.empty() || !std::isalnum(import_check[0]))) {
        import_check = import_check.ltrim();
        // Only treat as import if there's no semicolon in what follows.
        auto rest_of_line = llvm::StringRef(text).slice(line_start, line_end);
        if(!rest_of_line.contains(';')) {
            return {CompletionContext::Import, import_check.str()};
        }
    }

    return {};
}

et::serde::RawValue MasterServer::complete_include(const PreambleCompletionContext& ctx,
                                                   llvm::StringRef path) {
    std::string directory;
    std::vector<std::string> arguments;
    if(!compiler.fill_compile_args(path, directory, arguments))
        return et::serde::RawValue{"[]"};

    // Convert arguments to const char* array.
    std::vector<const char*> args_ptrs;
    args_ptrs.reserve(arguments.size());
    for(auto& arg: arguments) {
        args_ptrs.push_back(arg.c_str());
    }

    auto config = extract_search_config(args_ptrs, directory);
    DirListingCache dir_cache;
    auto resolved = resolve_search_config(config, dir_cache);

    // Determine search range based on context.
    unsigned start_idx = 0;
    if(ctx.kind == CompletionContext::IncludeAngled) {
        start_idx = resolved.angled_start_idx;
    }

    // Split prefix into dir_prefix and file_prefix if it contains '/'.
    llvm::StringRef prefix_ref(ctx.prefix);
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = prefix_ref;
    auto slash_pos = prefix_ref.rfind('/');
    if(slash_pos != llvm::StringRef::npos) {
        dir_prefix = prefix_ref.slice(0, slash_pos);
        file_prefix = prefix_ref.slice(slash_pos + 1, llvm::StringRef::npos);
    }

    std::vector<protocol::CompletionItem> items;
    llvm::StringSet<> seen;  // Deduplicate entries across search dirs.

    for(unsigned i = start_idx; i < resolved.dirs.size(); ++i) {
        auto& search_dir = resolved.dirs[i];

        // If there's a dir_prefix, resolve the subdirectory.
        const llvm::StringSet<>* entries = nullptr;
        if(!dir_prefix.empty()) {
            llvm::SmallString<256> sub_path(search_dir.path);
            llvm::sys::path::append(sub_path, dir_prefix);
            entries = resolve_dir(sub_path, dir_cache);
        } else {
            entries = search_dir.entries;
        }

        if(!entries)
            continue;

        for(auto& entry: *entries) {
            auto name = entry.getKey();
            if(!name.starts_with(file_prefix))
                continue;
            if(!seen.insert(name).second)
                continue;

            // Check if this entry is a directory.
            llvm::SmallString<256> full_path(search_dir.path);
            if(!dir_prefix.empty()) {
                llvm::sys::path::append(full_path, dir_prefix);
            }
            llvm::sys::path::append(full_path, name);

            bool is_dir = false;
            llvm::sys::fs::is_directory(llvm::Twine(full_path), is_dir);

            protocol::CompletionItem item;
            if(is_dir) {
                item.label = (name + "/").str();
            } else {
                item.label = name.str();
            }
            item.kind = protocol::CompletionItemKind::File;
            items.push_back(std::move(item));
        }
    }

    auto json = et::serde::json::to_json<et::ipc::lsp_config>(items);
    return et::serde::RawValue{json ? std::move(*json) : "[]"};
}

et::serde::RawValue MasterServer::complete_import(const PreambleCompletionContext& ctx) {
    std::vector<protocol::CompletionItem> items;
    llvm::StringRef prefix_ref(ctx.prefix);

    for(auto& [path_id, module_name]: compiler.module_map()) {
        llvm::StringRef name_ref(module_name);
        if(!name_ref.starts_with(prefix_ref))
            continue;

        protocol::CompletionItem item;
        item.label = module_name;
        item.kind = protocol::CompletionItemKind::Module;
        item.insert_text = module_name + ";";
        items.push_back(std::move(item));
    }

    auto json = et::serde::json::to_json<et::ipc::lsp_config>(items);
    return et::serde::RawValue{json ? std::move(*json) : "[]"};
}

// =========================================================================
// Forwarding helpers
// =========================================================================

using serde_raw = et::serde::RawValue;

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id)) {
        co_return serde_raw{"null"};
    }

    // After ensure_compiled returns, a new didChange may have arrived making
    // the AST stale again.  Sending a feature request with stale state is
    // wasteful and — more importantly — the queued IPC writes can fill up
    // the pipe buffer and deadlock the worker.  Drop the request instead.
    auto dit = documents.find(path_id);
    if(dit != documents.end() && dit->second.ast_dirty) {
        co_return serde_raw{"null"};
    }

    WorkerParams wp;
    wp.path = path;

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri,
                                                       const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id)) {
        co_return serde_raw{"null"};
    }

    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end()) {
        co_return serde_raw{"null"};
    }

    // Drop stale requests — see comment in the other overload.
    if(doc_it->second.ast_dirty) {
        co_return serde_raw{"null"};
    }

    WorkerParams wp;
    wp.path = path;

    lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateless(const std::string& uri,
                                                        const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    LOG_DEBUG("forward_stateless: {} path={} pos={}:{}",
              "request",
              path,
              position.line,
              position.character);

    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end()) {
        LOG_DEBUG("forward_stateless: doc not found for {}", path);
        co_return serde_raw{};
    }

    auto& doc = doc_it->second;

    WorkerParams wp;
    wp.path = path;
    wp.version = doc.version;
    wp.text = doc.text;
    if(!compiler.fill_compile_args(path, wp.directory, wp.arguments)) {
        LOG_DEBUG("forward_stateless: no CDB for {}", path);
        co_return serde_raw{};
    }

    // Ensure module deps, PCH, and PCM paths are ready for stateless compilation.
    if(!co_await compiler.ensure_deps(path_id, path, wp.text, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        LOG_DEBUG("forward_stateless: ensure_deps failed for {}", path);
        co_return serde_raw{};
    }

    lsp::PositionMapper mapper(wp.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value()) {
        LOG_DEBUG("forward_stateless: worker error for {}: {}", path, result.error().message);
        co_return serde_raw{};
    }
    LOG_DEBUG("forward_stateless: done {}", path);
    co_return std::move(result.value());
}

// Serialize a value to a JSON RawValue using LSP config.
template <typename T>
static serde_raw to_raw(const T& value) {
    auto json = et::serde::json::to_json<et::ipc::lsp_config>(value);
    return serde_raw{json ? std::move(*json) : "null"};
}



void MasterServer::register_handlers() {
    using StringVec = std::vector<std::string>;

    // === initialize ===
    peer.on_request([this](RequestContext& ctx, const protocol::InitializeParams& params)
                        -> RequestResult<protocol::InitializeParams> {
        if(lifecycle != ServerLifecycle::Uninitialized) {
            co_return et::outcome_error(protocol::Error{"Server already initialized"});
        }

        // Extract workspace root
        auto& init = params.lsp__initialize_params;
        if(init.root_uri.has_value()) {
            workspace_root = uri_to_path(*init.root_uri);
        }

        lifecycle = ServerLifecycle::Initialized;

        LOG_INFO("Initialized with workspace: {}", workspace_root);

        // Build capabilities
        protocol::InitializeResult result;
        auto& caps = result.capabilities;

        // Text document sync: incremental
        caps.text_document_sync = protocol::TextDocumentSyncOptions{
            .open_close = true,
            .change = protocol::TextDocumentSyncKind::Incremental,
            .save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true},
        };
        // watch workspace folder changes.
        caps.workspace = protocol::WorkspaceOptions{};
        caps.workspace->workspace_folders = protocol::WorkspaceFoldersServerCapabilities{
            .supported = true,
            .change_notifications = true,
        };

        // Feature capabilities
        caps.hover_provider = true;
        caps.completion_provider = protocol::CompletionOptions{
            .trigger_characters = StringVec{".", "<", ">", ":", "\"", "/", "*"},
        };
        caps.signature_help_provider = protocol::SignatureHelpOptions{
            .trigger_characters = StringVec{"(", ")", "{", "}", "<", ">", ","},
        };
        /// FIXME: In the future, we would support work done progress.
        caps.declaration_provider = protocol::DeclarationOptions{
            .work_done_progress = false,
        };
        caps.definition_provider = protocol::DefinitionOptions{
            .work_done_progress = false,
        };
        caps.implementation_provider = protocol::ImplementationOptions{
            .work_done_progress = false,
        };
        caps.type_definition_provider = protocol::TypeDefinitionOptions{
            .work_done_progress = false,
        };
        caps.references_provider = protocol::ReferenceOptions{
            .work_done_progress = false,
        };
        caps.document_symbol_provider = true;
        caps.document_link_provider = protocol::DocumentLinkOptions{};
        caps.code_action_provider = true;
        caps.folding_range_provider = true;
        caps.inlay_hint_provider = true;
        caps.call_hierarchy_provider = true;
        caps.type_hierarchy_provider = true;
        caps.workspace_symbol_provider = true;

        // Semantic tokens
        protocol::SemanticTokensOptions sem_opts;
        {
            auto lower_first = [](std::string_view name) -> std::string {
                std::string s(name);
                if(!s.empty()) {
                    s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
                }
                return s;
            };

            auto to_names = [&](auto names) {
                return std::ranges::to<std::vector>(names | std::views::transform(lower_first));
            };

            sem_opts.legend = protocol::SemanticTokensLegend{
                to_names(refl::reflection<SymbolKind::Kind>::member_names),
                to_names(refl::reflection<SymbolModifiers::Kind>::member_names),
            };
        }
        sem_opts.full = true;
        result.capabilities.semantic_tokens_provider = std::move(sem_opts);

        // Server info
        protocol::ServerInfo info;
        info.name = "clice";
        info.version = "0.1.0";
        result.server_info = std::move(info);

        co_return result;
    });

    // === initialized ===
    peer.on_notification([this](const protocol::InitializedParams& params) {
        // Load configuration from workspace
        config = CliceConfig::load_from_workspace(workspace_root);

        // Switch master to file logging under a session-timestamped directory
        if(!config.logging_dir.empty()) {
            auto now = std::chrono::system_clock::now();
            auto pid = llvm::sys::Process::getProcessId();
            auto session_dir =
                path::join(config.logging_dir, std::format("{:%Y-%m-%d_%H-%M-%S}_{}", now, pid));
            logging::file_logger("master", session_dir, logging::options);
            session_log_dir = session_dir;
        }

        LOG_INFO("Server ready (stateful={}, stateless={}, idle={}ms)",
                 config.stateful_worker_count,
                 config.stateless_worker_count,
                 config.idle_timeout_ms);

        // Start worker pool
        WorkerPoolOptions pool_opts;
        pool_opts.self_path = self_path;
        pool_opts.stateful_count = config.stateful_worker_count;
        pool_opts.stateless_count = config.stateless_worker_count;
        pool_opts.worker_memory_limit = config.worker_memory_limit;
        pool_opts.log_dir = session_log_dir;
        if(!pool.start(pool_opts)) {
            LOG_ERROR("Failed to start worker pool");
            return;
        }

        lifecycle = ServerLifecycle::Ready;

        // Load CDB in background
        loop.schedule(load_workspace());
    });

    // === shutdown ===
    peer.on_request(
        [this](RequestContext& ctx,
               const protocol::ShutdownParams& params) -> RequestResult<protocol::ShutdownParams> {
            lifecycle = ServerLifecycle::ShuttingDown;
            LOG_INFO("Shutdown requested");
            co_return nullptr;
        });

    // === exit ===
    peer.on_notification([this](const protocol::ExitParams& params) {
        lifecycle = ServerLifecycle::Exited;
        LOG_INFO("Exit notification received");

        // Persist index and cache state before stopping.
        indexer.save(config.index_dir);
        compiler.save_cache();

        // Graceful shutdown: cancel compilations, stop workers, then stop loop
        loop.schedule([this]() -> et::task<> {
            co_await pool.stop();
            loop.stop();
        }());
    });

    // === textDocument/didOpen ===
    peer.on_notification([this](const protocol::DidOpenTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto& td = params.text_document;
        auto path = uri_to_path(td.uri);
        auto path_id = path_pool.intern(path);

        auto& doc = documents[path_id];
        doc.version = td.version;
        doc.text = td.text;
        doc.generation++;

        LOG_DEBUG("didOpen: {} (v{})", path, td.version);
    });

    // === textDocument/didChange ===
    peer.on_notification([this](const protocol::DidChangeTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        auto it = documents.find(path_id);
        if(it == documents.end())
            return;

        auto& doc = it->second;
        doc.version = params.text_document.version;

        // Apply content changes.
        for(auto& change: params.content_changes) {
            std::visit(
                [&](auto& c) {
                    using T = std::remove_cvref_t<decltype(c)>;
                    if constexpr(std::is_same_v<T,
                                                protocol::TextDocumentContentChangeWholeDocument>) {
                        doc.text = c.text;
                    } else {
                        // Incremental change: replace range
                        auto& range = c.range;
                        lsp::PositionMapper mapper(doc.text, lsp::PositionEncoding::UTF16);
                        auto start = mapper.to_offset(range.start);
                        auto end = mapper.to_offset(range.end);
                        if(start && end && *start <= *end) {
                            doc.text.replace(*start, *end - *start, c.text);
                        }
                    }
                },
                change);
        }

        doc.generation++;
        doc.ast_dirty = true;

        LOG_DEBUG("didChange: path={} version={} gen={}", path, doc.version, doc.generation);

        // Notify the owning stateful worker so it marks the document dirty
        worker::DocumentUpdateParams update;
        update.path = path;
        update.version = doc.version;
        pool.notify_stateful(path_id, update);
    });

    // === textDocument/didClose ===
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        // Clean up compilation state and open file index.
        compiler.on_file_closed(path_id);
        indexer.remove_open_file(path_id, path);
        documents.erase(path_id);

        // Queue for background indexing to produce a proper MergedIndex shard.
        index_queue.push_back(path_id);
        schedule_indexing();

        // Clear diagnostics for closed file
        clear_diagnostics(params.text_document.uri);

        LOG_DEBUG("didClose: {}", path);
    });

    // === textDocument/didSave ===
    peer.on_notification([this](const protocol::DidSaveTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        // Invalidate artifacts and cascade to dependents.
        auto dirtied = compiler.on_file_saved(path_id);
        for(auto dirty_id: dirtied) {
            auto doc_it = documents.find(dirty_id);
            if(doc_it != documents.end()) {
                doc_it->second.ast_dirty = true;
            } else {
                index_queue.push_back(dirty_id);
            }
        }

        // Invalidate header contexts whose host is the saved file.
        llvm::SmallVector<std::uint32_t, 4> stale_headers;
        compiler.invalidate_host_contexts(path_id, stale_headers);
        for(auto hdr_id: stale_headers) {
            auto doc_it = documents.find(hdr_id);
            if(doc_it != documents.end()) {
                doc_it->second.ast_dirty = true;
                LOG_DEBUG("didSave: invalidated header context for path_id={}", hdr_id);
            }
        }

        // Trigger background indexing after save.
        schedule_indexing();

        LOG_DEBUG("didSave: {}", params.text_document.uri);
    });

    // =========================================================================
    // Feature requests routed to stateful workers (RawValue passthrough)
    // =========================================================================

    // --- textDocument/hover ---
    peer.on_request([this](RequestContext& ctx, const protocol::HoverParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::HoverParams>(
            params.text_document_position_params.text_document.uri,
            params.text_document_position_params.position);
    });

    // --- textDocument/semanticTokens/full ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::SemanticTokensParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::SemanticTokensParams>(params.text_document.uri);
    });

    // --- textDocument/inlayHint ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::InlayHintParams& params) -> RawResult {
            co_return co_await forward_stateful<worker::InlayHintsParams>(params.text_document.uri);
        });

    // --- textDocument/foldingRange ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::FoldingRangeParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::FoldingRangeParams>(params.text_document.uri);
    });

    // --- textDocument/documentSymbol ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentSymbolParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::DocumentSymbolParams>(params.text_document.uri);
    });

    // --- textDocument/documentLink ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::DocumentLinkParams& params) -> RawResult {
        co_return co_await forward_stateful<worker::DocumentLinkParams>(params.text_document.uri);
    });

    // --- textDocument/codeAction ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CodeActionParams& params) -> RawResult {
            co_return co_await forward_stateful<worker::CodeActionParams>(params.text_document.uri);
        });

    // --- textDocument/definition ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::DefinitionParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto path = uri_to_path(uri);
            auto path_id = path_pool.intern(path);
            auto doc_it = documents.find(path_id);
            const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

            // Try index-based lookup first.
            auto result = indexer.query_relations(path, path_id, pos, RelationKind::Definition, doc_text);
            if(!result.empty() && result.data != "null") {
                co_return std::move(result);
            }

            // Fall back to stateful worker AST query.
            co_return co_await forward_stateful<worker::GoToDefinitionParams>(uri, pos);
        });

    // --- textDocument/references ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;
            auto path = uri_to_path(uri);
            auto path_id = path_pool.intern(path);
            auto doc_it = documents.find(path_id);
            const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

            auto refs = indexer.query_relations(path, path_id, pos, RelationKind::Reference, doc_text);

            if(params.context.include_declaration) {
                auto defs = indexer.query_relations(path, path_id, pos, RelationKind::Definition, doc_text);
                if(!defs.empty() && defs.data != "null") {
                    if(refs.empty() || refs.data == "null") {
                        co_return std::move(defs);
                    }
                    // Merge JSON arrays.
                    if(refs.data.size() > 2 && defs.data.size() > 2) {
                        std::string merged = refs.data.substr(0, refs.data.size() - 1);
                        merged += ',';
                        merged += defs.data.substr(1);
                        co_return serde_raw{std::move(merged)};
                    }
                }
            }

            co_return std::move(refs);
        });

    // --- textDocument/typeDefinition ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::TypeDefinitionParams& params) -> RawResult {
            co_return serde_raw{"null"};  // not supported yet
        });

    // --- textDocument/implementation ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ImplementationParams& params) -> RawResult {
            co_return serde_raw{"null"};  // not supported yet
        });

    // --- textDocument/declaration ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::DeclarationParams& params) -> RawResult {
            co_return serde_raw{"null"};  // not supported yet
        });

    // =========================================================================
    // Feature requests routed to stateless workers
    // =========================================================================

    // --- textDocument/completion ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            auto uri = params.text_document_position_params.text_document.uri;
            auto position = params.text_document_position_params.position;

            // Check if cursor is on an #include or import line.
            auto path = uri_to_path(uri);
            auto path_id = path_pool.intern(path);
            auto doc_it = documents.find(path_id);
            if(doc_it != documents.end()) {
                lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
                auto offset = mapper.to_offset(position);
                if(offset) {
                    auto pctx = detect_completion_context(doc_it->second.text, *offset);
                    if(pctx.kind == CompletionContext::IncludeQuoted ||
                       pctx.kind == CompletionContext::IncludeAngled) {
                        co_return complete_include(pctx, path);
                    }
                    if(pctx.kind == CompletionContext::Import) {
                        co_return complete_import(pctx);
                    }
                }
            }

            // Default: forward to stateless worker.
            co_return co_await forward_stateless<worker::CompletionParams>(uri, position);
        });

    // --- textDocument/signatureHelp ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::SignatureHelpParams& params) -> RawResult {
            co_return co_await forward_stateless<worker::SignatureHelpParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
        });

    // =========================================================================
    // Hierarchy and workspace symbol handlers (index-based)
    // =========================================================================

    // --- textDocument/prepareCallHierarchy ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto path = uri_to_path(uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.lookup_symbol(uri, path, path_id, pos, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method))
            co_return serde_raw{"null"};

        std::vector<protocol::CallHierarchyItem> items;
        items.push_back(Indexer::build_call_hierarchy_item(*info));
        co_return to_raw(items);
    });

    // --- callHierarchy/incomingCalls ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        auto path = uri_to_path(params.item.uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.resolve_hierarchy_item(
            params.item.uri, path, path_id, params.item.range, params.item.data, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        co_return indexer.find_incoming_calls(info->hash);
    });

    // --- callHierarchy/outgoingCalls ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto path = uri_to_path(params.item.uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.resolve_hierarchy_item(
            params.item.uri, path, path_id, params.item.range, params.item.data, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        co_return indexer.find_outgoing_calls(info->hash);
    });

    // --- textDocument/prepareTypeHierarchy ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchyPrepareParams& params) -> RawResult {
        auto& uri = params.text_document_position_params.text_document.uri;
        auto& pos = params.text_document_position_params.position;
        auto path = uri_to_path(uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.lookup_symbol(uri, path, path_id, pos, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
             info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
            co_return serde_raw{"null"};

        std::vector<protocol::TypeHierarchyItem> items;
        items.push_back(Indexer::build_type_hierarchy_item(*info));
        co_return to_raw(items);
    });

    // --- typeHierarchy/supertypes ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
        auto path = uri_to_path(params.item.uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.resolve_hierarchy_item(
            params.item.uri, path, path_id, params.item.range, params.item.data, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        co_return indexer.find_supertypes(info->hash);
    });

    // --- typeHierarchy/subtypes ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
        auto path = uri_to_path(params.item.uri);
        auto path_id = path_pool.intern(path);
        auto doc_it = documents.find(path_id);
        const std::string* doc_text = doc_it != documents.end() ? &doc_it->second.text : nullptr;

        auto info = indexer.resolve_hierarchy_item(
            params.item.uri, path, path_id, params.item.range, params.item.data, doc_text);
        if(!info)
            co_return serde_raw{"null"};

        co_return indexer.find_subtypes(info->hash);
    });

    // --- workspace/symbol ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::WorkspaceSymbolParams& params) -> RawResult {
        co_return indexer.search_symbols(params.query);
    });

    // === clice/ Extension Commands ===

    // --- clice/queryContext ---
    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto path = uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);
            int offset_val = std::max(0, params.offset.value_or(0));
            constexpr int page_size = 10;

            ext::QueryContextResult result;

            std::vector<ext::ContextItem> all_items;

            // For headers: find source files that transitively include this file.
            auto hosts = dependency_graph.find_host_sources(path_id);
            for(auto host_id: hosts) {
                auto host_path = path_pool.resolve(host_id);
                auto host_cdb = cdb.lookup(host_path, {.suppress_logging = true});
                if(host_cdb.empty())
                    continue;
                auto host_uri_opt = lsp::URI::from_file_path(std::string(host_path));
                if(!host_uri_opt)
                    continue;
                ext::ContextItem item;
                item.label = llvm::sys::path::filename(host_path).str();
                item.description = std::string(host_path);
                item.uri = host_uri_opt->str();
                all_items.push_back(std::move(item));
            }

            // For source files: list distinct CDB entries (e.g. debug/release).
            if(hosts.empty()) {
                auto entries = cdb.lookup(path, {.suppress_logging = true});
                for(std::size_t i = 0; i < entries.size(); ++i) {
                    auto& entry = entries[i];
                    // Build a description from distinguishing flags.
                    std::string desc;
                    for(std::size_t j = 0; j < entry.arguments.size(); ++j) {
                        llvm::StringRef a(entry.arguments[j]);
                        if(a.starts_with("-D") || a.starts_with("-O") || a.starts_with("-std=") ||
                           a.starts_with("-g")) {
                            if(!desc.empty())
                                desc += ' ';
                            desc += entry.arguments[j];
                            // Handle split args like "-D" "CONFIG_A"
                            if((a == "-D" || a == "-O") && j + 1 < entry.arguments.size()) {
                                desc += entry.arguments[++j];
                            }
                        }
                    }
                    if(desc.empty())
                        desc = std::format("config #{}", i);

                    auto uri_opt = lsp::URI::from_file_path(std::string(path));
                    if(!uri_opt)
                        continue;
                    ext::ContextItem item;
                    item.label = desc;
                    item.description = entry.directory.str();
                    item.uri = uri_opt->str();
                    all_items.push_back(std::move(item));
                }
            }

            result.total = static_cast<int>(all_items.size());
            int end = std::min(offset_val + page_size, static_cast<int>(all_items.size()));
            for(int i = offset_val; i < end; ++i) {
                result.contexts.push_back(std::move(all_items[i]));
            }
            co_return to_raw(result);
        });

    // --- clice/currentContext ---
    peer.on_request(
        "clice/currentContext",
        [this](RequestContext& ctx, const ext::CurrentContextParams& params) -> RawResult {
            auto path = uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);

            ext::CurrentContextResult result;

            auto active_ctx = compiler.get_active_context(path_id);
            if(active_ctx) {
                auto ctx_path = path_pool.resolve(*active_ctx);
                auto ctx_uri_opt = lsp::URI::from_file_path(std::string(ctx_path));
                if(ctx_uri_opt) {
                    ext::ContextItem item;
                    item.label = llvm::sys::path::filename(ctx_path).str();
                    item.description = std::string(ctx_path);
                    item.uri = ctx_uri_opt->str();
                    result.context = std::move(item);
                }
            }
            co_return to_raw(result);
        });

    // --- clice/switchContext ---
    peer.on_request(
        "clice/switchContext",
        [this](RequestContext& ctx, const ext::SwitchContextParams& params) -> RawResult {
            auto path = uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);
            auto context_path = uri_to_path(params.context_uri);
            auto context_path_id = path_pool.intern(context_path);

            ext::SwitchContextResult result;

            // Verify the context file has a CDB entry.
            auto context_cdb = cdb.lookup(context_path, {.suppress_logging = true});
            if(context_cdb.empty()) {
                result.success = false;
                co_return to_raw(result);
            }

            // Set active context and invalidate cached state.
            compiler.switch_context(path_id, context_path_id);

            // Mark the document as dirty so it gets recompiled.
            auto doc_it = documents.find(path_id);
            if(doc_it != documents.end()) {
                doc_it->second.ast_dirty = true;
            }

            result.success = true;
            co_return to_raw(result);
        });
}

}  // namespace clice
