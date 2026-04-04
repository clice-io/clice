#include "server/master_server.h"

#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "eventide/ipc/json_codec.h"
#include "eventide/ipc/lsp/position.h"
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
#include "syntax/scan.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace protocol = eventide::ipc::protocol;
namespace lsp = eventide::ipc::lsp;
namespace refl = eventide::refl;
using et::ipc::RequestResult;
using RequestContext = et::ipc::JsonPeer::RequestContext;

/// Hash a file's content using xxh3_64bits. Returns 0 on read failure.
static std::uint64_t hash_file(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if(!buf)
        return 0;
    return llvm::xxh3_64bits((*buf)->getBuffer());
}

/// Capture a two-layer staleness snapshot after a successful compilation.
/// Interns dependency paths into the PathPool and hashes each file's content.
static DepsSnapshot capture_deps_snapshot(PathPool& pool, llvm::ArrayRef<std::string> deps) {
    DepsSnapshot snap;
    // Capture timestamp BEFORE hashing to avoid TOCTOU: if a file is modified
    // during hashing, its mtime will be > build_at, triggering Layer 2 re-hash.
    snap.build_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    snap.path_ids.reserve(deps.size());
    snap.hashes.reserve(deps.size());
    for(const auto& file: deps) {
        snap.path_ids.push_back(pool.intern(file));
        snap.hashes.push_back(hash_file(file));
    }
    return snap;
}

/// Two-layer staleness check.
///
/// Layer 1 (fast): stat each dep file, compare mtime against build_at.
///   If all mtimes <= build_at → nothing changed, return false immediately.
///
/// Layer 2 (precise): for files with mtime > build_at, re-hash their content.
///   If the hash matches the stored hash → file was touched but not modified.
///   If any hash differs → truly changed, return true.
static bool deps_changed(const PathPool& pool, const DepsSnapshot& snap) {
    for(std::size_t i = 0; i < snap.path_ids.size(); ++i) {
        auto path = pool.resolve(snap.path_ids[i]);
        llvm::sys::fs::file_status status;
        if(auto ec = llvm::sys::fs::status(path, status)) {
            // File disappeared — definitely changed.
            if(snap.hashes[i] != 0)
                return true;
            continue;
        }

        // Layer 1: mtime check (cheap, stat only).
        auto current_mtime = llvm::sys::toTimeT(status.getLastModificationTime());
        if(current_mtime <= snap.build_at)
            continue;

        // Layer 2: mtime is newer — re-hash content to confirm actual change.
        auto current_hash = hash_file(path);
        if(current_hash != snap.hashes[i])
            return true;
    }
    return false;
}

MasterServer::MasterServer(et::event_loop& loop, et::ipc::JsonPeer& peer, std::string self_path) :
    loop(loop), peer(peer), pool(loop), self_path(std::move(self_path)) {}

MasterServer::~MasterServer() {
    if(compile_graph) {
        compile_graph->cancel_all();
    }
}

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
    }

    // Search for compile_commands.json
    std::string cdb_path;

    // If the config specifies a CDB path, use it
    if(!config.compile_commands_path.empty()) {
        if(llvm::sys::fs::exists(config.compile_commands_path)) {
            cdb_path = config.compile_commands_path;
        } else {
            LOG_WARN("Configured compile_commands_path not found: {}",
                     config.compile_commands_path);
        }
    }

    // Otherwise auto-detect in common locations
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

    auto unresolved = report.includes_found - report.includes_resolved;
    double accuracy =
        report.includes_found > 0
            ? 100.0 * static_cast<double>(report.includes_resolved) / report.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), " "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        report.elapsed_ms,
        report.total_files,
        report.source_files,
        report.header_files,
        report.total_edges,
        report.includes_resolved,
        report.includes_found,
        accuracy,
        report.waves);
    if(unresolved > 0) {
        LOG_WARN("{} unresolved includes", unresolved);
    }

    // Build reverse mapping: path_id -> module name.
    for(auto& [module_name, path_ids]: dependency_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }

    // Load persisted index from disk.
    load_index();

    // Build index queue from CDB entries (all source files).
    // CDB entries use the CDB's internal path_ids; convert to server path_ids.
    if(config.enable_indexing) {
        for(auto& entry: cdb.get_entries()) {
            auto file = cdb.resolve_path(entry.file);
            auto server_id = path_pool.intern(file);
            index_queue.push_back(server_id);
        }
        if(!index_queue.empty()) {
            LOG_INFO("Queued {} files for background indexing", index_queue.size());
            for(auto sid: index_queue) {
                LOG_INFO("  queue entry: server_path_id={} path='{}'", sid, path_pool.resolve(sid));
            }
            schedule_indexing();
        }
    }

    if(path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        co_return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = path_pool.resolve(path_id);
        auto results = cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty()) {
            return {};
        }

        auto& ctx = results[0];
        auto scan_result = scan_precise(ctx.arguments, ctx.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = dependency_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        // Module implementation units implicitly depend on their interface unit.
        if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
            auto mod_ids = dependency_graph.lookup_module(scan_result.module_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        return deps;
    };

    // Dispatch: sends BuildPCM request to a stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> et::task<bool> {
        auto mod_it = path_to_module.find(path_id);
        if(mod_it == path_to_module.end()) {
            co_return false;
        }

        auto file_path = std::string(path_pool.resolve(path_id));
        worker::BuildPCMParams pcm_params;
        pcm_params.file = file_path;
        if(!fill_compile_args(file_path, pcm_params.directory, pcm_params.arguments)) {
            co_return false;
        }
        pcm_params.module_name = mod_it->second;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        for(auto& [pid, pcm_path]: pcm_paths) {
            auto dep_mod_it = path_to_module.find(pid);
            if(dep_mod_it != path_to_module.end()) {
                pcm_params.pcms[dep_mod_it->second] = pcm_path;
            }
        }

        auto result = co_await pool.send_stateless(pcm_params);
        if(!result.has_value() || !result.value().success) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     mod_it->second,
                     result.has_value() ? result.value().error : result.error().message);
            co_return false;
        }

        pcm_paths[path_id] = result.value().pcm_path;
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, result.value().pcm_path);
        co_return true;
    };

    compile_graph = std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", path_to_module.size());
}

bool MasterServer::fill_compile_args(llvm::StringRef path,
                                     std::string& directory,
                                     std::vector<std::string>& arguments) {
    auto results = cdb.lookup(path, {.query_toolchain = true});
    if(results.empty()) {
        LOG_WARN("No CDB entry for {}", path);
        return false;
    }
    auto& ctx = results.front();
    directory = ctx.directory.str();
    arguments.clear();
    for(auto* arg: ctx.arguments) {
        arguments.emplace_back(arg);
    }
    return true;
}

et::task<bool> MasterServer::ensure_pch(std::uint32_t path_id,
                                        llvm::StringRef path,
                                        const std::string& text,
                                        const std::string& directory,
                                        const std::vector<std::string>& arguments) {
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        // No preamble directives — PCH would be empty. Clear any stale entry.
        auto it = pch_states.find(path_id);
        if(it != pch_states.end()) {
            fs::remove(it->second.path);
            pch_states.erase(it);
        }
        co_return true;
    }

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(text).substr(0, bound));

    // Reuse existing PCH if preamble content and deps haven't changed.
    if(auto it = pch_states.find(path_id); it != pch_states.end()) {
        auto& st = it->second;
        if(st.hash == preamble_hash && !st.path.empty() && !deps_changed(path_pool, st.deps)) {
            st.bound = bound;
            co_return true;
        }
    }

    // If another coroutine is already building PCH for this file, wait for it.
    if(auto it = pch_states.find(path_id); it != pch_states.end() && it->second.building) {
        co_await it->second.building->wait();
        co_return !pch_states[path_id].path.empty();
    }

    // Register in-flight build so concurrent requests wait on us.
    auto completion = std::make_shared<et::event>();
    pch_states[path_id].building = completion;

    // Build a new PCH via stateless worker.
    worker::BuildPCHParams pch_params;
    pch_params.file = std::string(path);
    pch_params.directory = directory;
    pch_params.arguments = arguments;
    pch_params.content = text;
    pch_params.preamble_bound = bound;

    LOG_DEBUG("Building PCH for {}, bound={}", path, bound);

    auto result = co_await pool.send_stateless(pch_params);

    if(!result.has_value() || !result.value().success) {
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        pch_states[path_id].building.reset();
        completion->set();
        co_return false;
    }

    // Delete old PCH temp file before replacing.
    auto& st = pch_states[path_id];
    if(!st.path.empty()) {
        fs::remove(st.path);
    }

    st.path = result.value().pch_path;
    st.bound = bound;
    st.hash = preamble_hash;
    st.deps = capture_deps_snapshot(path_pool, result.value().deps);
    st.building.reset();

    LOG_INFO("PCH built for {}: {}", path, result.value().pch_path);

    completion->set();
    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
et::task<bool> MasterServer::ensure_deps(std::uint32_t path_id,
                                         llvm::StringRef path,
                                         const std::string& text,
                                         const std::string& directory,
                                         const std::vector<std::string>& arguments,
                                         std::pair<std::string, uint32_t>& pch,
                                         std::unordered_map<std::string, std::string>& pcms) {
    // Compile C++20 module dependencies (PCMs).
    if(compile_graph && !co_await compile_graph->compile_deps(path_id)) {
        co_return false;
    }

    // Build or reuse PCH.
    auto pch_ok = co_await ensure_pch(path_id, path, text, directory, arguments);
    if(pch_ok) {
        if(auto pch_it = pch_states.find(path_id); pch_it != pch_states.end()) {
            pch = {pch_it->second.path, pch_it->second.bound};
        }
    }

    // Fill all available PCM paths so clang can resolve transitive imports.
    // Exclude the file's own PCM to avoid "multiple module declarations".
    for(auto& [pid, pcm_path]: pcm_paths) {
        if(pid == path_id)
            continue;
        auto mod_it = path_to_module.find(pid);
        if(mod_it != path_to_module.end()) {
            pcms[mod_it->second] = pcm_path;
        }
    }

    co_return true;
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
/// each call ensure_compiled(). The first one triggers the actual compilation;
/// subsequent ones observe ast_dirty == false after the first completes and
/// take the fast path. This is safe because forward_stateful serialises
/// requests per worker slot.
et::task<bool> MasterServer::ensure_compiled(std::uint32_t path_id) {
    auto it = documents.find(path_id);
    if(it == documents.end()) {
        co_return false;
    }

    auto& doc = it->second;

    // Fast path: AST was previously compiled successfully.
    // Check if any dependency file has changed since the last compilation.
    // We check both AST deps (body includes) and PCH deps (preamble includes),
    // because when PCH is active the preamble headers are baked into the PCH
    // and won't appear in the AST's directive list.
    if(!doc.ast_dirty) {
        bool changed = false;
        auto ast_deps_it = ast_deps.find(path_id);
        if(ast_deps_it != ast_deps.end() && deps_changed(path_pool, ast_deps_it->second)) {
            changed = true;
        }
        if(!changed) {
            auto pch_it = pch_states.find(path_id);
            if(pch_it != pch_states.end() && deps_changed(path_pool, pch_it->second.deps)) {
                changed = true;
            }
        }
        if(!changed) {
            co_return true;
        }
        doc.ast_dirty = true;
    }

    // Snapshot the generation counter *before* any co_await.  After compilation
    // we compare it with the current value to detect edits that arrived while
    // we were suspended — if they differ, the result is stale and we must not
    // mark the AST as clean.
    auto gen = doc.generation;

    auto file_path = std::string(path_pool.resolve(path_id));
    auto uri = lsp::URI::from_file_path(file_path);
    std::string uri_str = uri.has_value() ? uri->str() : file_path;

    // After co_await suspension points the iterator may be invalidated (the
    // documents map could have been modified by didClose on another file).
    auto recheck = [&]() -> bool {
        it = documents.find(path_id);
        return it != documents.end();
    };

    // ── Phase 1–3: Module deps, PCH, PCM paths ─────────────────────────
    worker::CompileParams params;
    params.path = file_path;
    if(!recheck())
        co_return false;
    params.version = it->second.version;
    params.text = it->second.text;
    if(!fill_compile_args(path_pool.resolve(path_id), params.directory, params.arguments)) {
        co_return false;
    }

    if(!co_await ensure_deps(path_id,
                             params.path,
                             params.text,
                             params.directory,
                             params.arguments,
                             params.pch,
                             params.pcms)) {
        LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
        co_return false;
    }

    if(!recheck())
        co_return false;

    // ── Phase 4: Dispatch to stateful worker ────────────────────────────
    //
    // The stateful worker receives the full document text and compile args,
    // builds the AST, and caches it for subsequent feature requests.  The
    // response carries diagnostics collected during compilation.
    LOG_DEBUG("Sending compile: path={}, args={}, gen={}",
              params.path,
              params.arguments.size(),
              gen);

    auto result = co_await pool.send_stateful(path_id, params);

    // Re-lookup: the document may have been closed while we were compiling.
    it = documents.find(path_id);
    if(it == documents.end())
        co_return false;

    auto& doc2 = it->second;

    // ── Phase 5: Handle result ──────────────────────────────────────────
    //
    // Generation mismatch means the user edited the file while we compiled.
    // The AST we just built corresponds to an older version of the text, so
    // we discard the diagnostics and leave ast_dirty == true.  The next
    // feature request will trigger another compile cycle with the latest text.
    if(doc2.generation != gen) {
        if(result.has_value()) {
            LOG_DEBUG("Generation mismatch ({} vs {}), dropping diagnostics for {}",
                      doc2.generation,
                      gen,
                      uri_str);
        }
        co_return false;
    }

    if(!result.has_value()) {
        LOG_WARN("Compile failed for {}: {}", uri_str, result.error().message);
        // Clear stale diagnostics so the editor doesn't show errors from a
        // previous successful compilation that no longer apply.
        clear_diagnostics(uri_str);
        co_return false;
    }

    publish_diagnostics(uri_str, doc2.version, result.value().diagnostics);
    doc2.ast_dirty = false;
    ast_deps[path_id] = capture_deps_snapshot(path_pool, result.value().deps);
    schedule_indexing();
    co_return true;
}

// =========================================================================
// Index integration
// =========================================================================

void MasterServer::merge_index_result(const void* tu_index_data, std::size_t size) {
    auto tu_index = index::TUIndex::from(tu_index_data);

    // Merge symbols into ProjectIndex, get TU-local path_id -> global path_id mapping.
    auto file_ids_map = project_index.merge(tu_index);

    auto main_tu_path_id = static_cast<std::uint32_t>(tu_index.graph.paths.size() - 1);

    // Merge a single file's index into the corresponding MergedIndex shard.
    auto merge_file_index = [&](std::uint32_t tu_path_id, index::FileIndex& file_idx) {
        auto global_path_id = file_ids_map[tu_path_id];
        auto& merged = merged_indices[global_path_id];

        if(tu_path_id == main_tu_path_id) {
            // Main file (source file) gets a compilation context with include locations.
            // Collect ALL include locations with path_ids remapped to project-level ids.
            std::vector<index::IncludeLocation> include_locs;
            for(auto& loc: tu_index.graph.locations) {
                index::IncludeLocation remapped = loc;
                remapped.path_id = file_ids_map[loc.path_id];
                include_locs.push_back(remapped);
            }
            // Read the file content from disk for position mapping in queries.
            auto file_path = project_index.path_pool.path(global_path_id);
            llvm::StringRef file_content;
            std::string file_content_storage;
            auto buf = llvm::MemoryBuffer::getFile(file_path);
            if(buf) {
                file_content_storage = (*buf)->getBuffer().str();
                file_content = file_content_storage;
            }
            merged.merge(global_path_id,
                         tu_index.built_at,
                         std::move(include_locs),
                         file_idx,
                         file_content);
        } else {
            // Header files get a header context keyed by include location.
            std::uint32_t include_id = 0;
            for(std::uint32_t i = 0; i < tu_index.graph.locations.size(); ++i) {
                if(tu_index.graph.locations[i].path_id == tu_path_id) {
                    include_id = i;
                    break;
                }
            }
            // Read header file content for position mapping in queries.
            auto header_path = project_index.path_pool.path(global_path_id);
            llvm::StringRef header_content;
            std::string header_content_storage;
            auto header_buf = llvm::MemoryBuffer::getFile(header_path);
            if(header_buf) {
                header_content_storage = (*header_buf)->getBuffer().str();
                header_content = header_content_storage;
            }
            merged.merge(global_path_id, include_id, file_idx, header_content);
        }
    };

    // Merge from path_file_indices (deserialized TUIndex from IPC).
    for(auto& [tu_path_id, file_idx]: tu_index.path_file_indices) {
        merge_file_index(tu_path_id, file_idx);
    }

    // Merge main file index.
    merge_file_index(main_tu_path_id, tu_index.main_file_index);

    LOG_INFO("Merged TUIndex: {} paths, {} symbols, {} merged_shards",
             tu_index.graph.paths.size(),
             tu_index.symbols.size(),
             merged_indices.size());
    for(auto& [pid, _]: merged_indices) {
        LOG_INFO("  shard proj_path_id={} path='{}'", pid, project_index.path_pool.path(pid));
    }
}

void MasterServer::save_index() {
    if(config.index_dir.empty())
        return;

    auto ec = llvm::sys::fs::create_directories(config.index_dir);
    if(ec) {
        LOG_WARN("Failed to create index directory {}: {}", config.index_dir, ec.message());
        return;
    }

    // Save ProjectIndex.
    auto project_path = path::join(config.index_dir, "project.idx");
    {
        std::error_code write_ec;
        llvm::raw_fd_ostream os(project_path, write_ec);
        if(!write_ec) {
            project_index.serialize(os);
            LOG_INFO("Saved ProjectIndex to {}", project_path);
        } else {
            LOG_WARN("Failed to save ProjectIndex: {}", write_ec.message());
        }
    }

    // Save MergedIndex shards.
    auto shards_dir = path::join(config.index_dir, "shards");
    ec = llvm::sys::fs::create_directories(shards_dir);
    if(ec) {
        LOG_WARN("Failed to create shards directory: {}", ec.message());
        return;
    }

    std::size_t saved = 0;
    for(auto& [path_id, merged]: merged_indices) {
        if(!merged.need_rewrite())
            continue;
        auto shard_path = path::join(shards_dir, std::to_string(path_id) + ".idx");
        std::error_code write_ec;
        llvm::raw_fd_ostream os(shard_path, write_ec);
        if(!write_ec) {
            merged.serialize(os);
            ++saved;
        }
    }
    LOG_INFO("Saved {} MergedIndex shards (of {} total)", saved, merged_indices.size());
}

void MasterServer::load_index() {
    if(config.index_dir.empty())
        return;

    // Load ProjectIndex.
    auto project_path = path::join(config.index_dir, "project.idx");
    auto buf = llvm::MemoryBuffer::getFile(project_path);
    if(buf) {
        project_index = index::ProjectIndex::from((*buf)->getBufferStart());
        LOG_INFO("Loaded ProjectIndex: {} symbols", project_index.symbols.size());
    }

    // Load MergedIndex shards.
    auto shards_dir = path::join(config.index_dir, "shards");
    std::error_code ec;
    for(auto it = llvm::sys::fs::directory_iterator(shards_dir, ec);
        !ec && it != llvm::sys::fs::directory_iterator();
        it.increment(ec)) {
        auto filename = llvm::sys::path::filename(it->path());
        if(!filename.ends_with(".idx"))
            continue;

        auto stem = filename.drop_back(4);  // remove ".idx"
        std::uint32_t path_id = 0;
        if(stem.getAsInteger(10, path_id))
            continue;

        merged_indices[path_id] = index::MergedIndex::load(it->path());
    }

    if(!merged_indices.empty()) {
        LOG_INFO("Loaded {} MergedIndex shards", merged_indices.size());
    }
}

void MasterServer::schedule_indexing() {
    LOG_INFO(
        "schedule_indexing called: enable={} active={} scheduled={} queue_size={} queue_pos={}",
        config.enable_indexing,
        indexing_active,
        indexing_scheduled,
        index_queue.size(),
        index_queue_pos);
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

        // Note: we do NOT skip open documents here.  Index data is needed for
        // cross-file features (references, call hierarchy, type hierarchy, etc.)
        // regardless of whether the file is open.

        // Check if the index needs update by checking mtime against existing shard.
        // If the file is not yet in the project_index path pool, it has never been
        // indexed — always proceed.  Only skip when we already have a shard that is
        // still fresh.
        auto cache_it = project_index.path_pool.find(file_path);
        if(cache_it != project_index.path_pool.cache.end()) {
            auto proj_path_id = cache_it->second;
            auto merged_it = merged_indices.find(proj_path_id);
            if(merged_it != merged_indices.end()) {
                // Build path mapping for need_update check.
                llvm::SmallVector<llvm::StringRef> path_mapping;
                for(auto& p: project_index.path_pool.paths) {
                    path_mapping.push_back(p);
                }
                if(!merged_it->second.need_update(path_mapping))
                    continue;
            }
        }

        // Prepare IndexParams for the stateless worker.
        worker::IndexParams params;
        params.file = file_path;
        if(!fill_compile_args(file_path, params.directory, params.arguments))
            continue;

        // Fill PCM deps for module-aware indexing.
        for(auto& [pid, pcm_path]: pcm_paths) {
            auto mod_it = path_to_module.find(pid);
            if(mod_it != path_to_module.end()) {
                params.pcms[mod_it->second] = pcm_path;
            }
        }

        LOG_INFO("Background indexing: {}", file_path);

        auto result = co_await pool.send_stateless(params);
        if(result.has_value() && result.value().success && !result.value().tu_index_data.empty()) {
            LOG_INFO("Background indexing got TUIndex for {}: {} bytes",
                     file_path,
                     result.value().tu_index_data.size());
            merge_index_result(result.value().tu_index_data.data(),
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
    save_index();
}

// =========================================================================
// Forwarding helpers
// =========================================================================

using serde_raw = et::serde::RawValue;

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id))
        co_return serde_raw{"null"};

    WorkerParams wp;
    wp.path = path;

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateful(const std::string& uri,
                                                       const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    if(!co_await ensure_compiled(path_id))
        co_return serde_raw{"null"};

    WorkerParams wp;
    wp.path = path;

    auto doc_it = documents.find(path_id);
    if(doc_it != documents.end()) {
        lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
        auto offset = mapper.to_offset(position);
        if(!offset)
            co_return serde_raw{"null"};
        wp.offset = *offset;
    }

    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
}

template <typename WorkerParams>
MasterServer::RawResult MasterServer::forward_stateless(const std::string& uri,
                                                        const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto path_id = path_pool.intern(path);

    auto doc_it = documents.find(path_id);
    if(doc_it == documents.end())
        co_return serde_raw{};

    auto& doc = doc_it->second;

    WorkerParams wp;
    wp.path = path;
    wp.version = doc.version;
    wp.text = doc.text;
    if(!fill_compile_args(path, wp.directory, wp.arguments))
        co_return serde_raw{};

    // Ensure module deps, PCH, and PCM paths are ready for stateless compilation.
    if(!co_await ensure_deps(path_id, path, wp.text, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        co_return serde_raw{};
    }

    lsp::PositionMapper mapper(wp.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value())
        co_return serde_raw{};
    co_return std::move(result.value());
}

// Serialize a value to a JSON RawValue using LSP config.
template <typename T>
static serde_raw to_raw(const T& value) {
    auto json = et::serde::json::to_json<et::ipc::lsp_config>(value);
    return serde_raw{json ? std::move(*json) : "null"};
}

MasterServer::RawResult MasterServer::query_index_relations(const std::string& uri,
                                                            const protocol::Position& position,
                                                            RelationKind kind) {
    auto path = uri_to_path(uri);
    auto server_path_id = path_pool.intern(path);

    // Need document text for position-to-offset conversion.
    auto doc_it = documents.find(server_path_id);
    if(doc_it == documents.end())
        co_return serde_raw{"null"};

    lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
    auto offset_opt = mapper.to_offset(position);
    if(!offset_opt)
        co_return serde_raw{"null"};
    auto offset = *offset_opt;

    // Find the project-level path_id for this file.
    auto proj_cache_it = project_index.path_pool.find(path);
    if(proj_cache_it == project_index.path_pool.cache.end()) {
        LOG_DEBUG("query_index_relations: path '{}' not in project_index", path);
        co_return serde_raw{"null"};
    }
    auto proj_path_id = proj_cache_it->second;

    // Lookup occurrence at offset in this file's MergedIndex.
    auto merged_it = merged_indices.find(proj_path_id);
    if(merged_it == merged_indices.end()) {
        LOG_DEBUG("query_index_relations: no MergedIndex for proj_path_id={}", proj_path_id);
        co_return serde_raw{"null"};
    }

    index::SymbolHash symbol_hash = 0;
    merged_it->second.lookup(offset, [&](const index::Occurrence& o) {
        symbol_hash = o.target;
        return false;  // stop after first match
    });

    if(symbol_hash == 0) {
        LOG_DEBUG("query_index_relations: no occurrence at offset {} in '{}'", offset, path);
        co_return serde_raw{"null"};
    }

    // Get reference files from ProjectIndex.
    auto sym_it = project_index.symbols.find(symbol_hash);
    if(sym_it == project_index.symbols.end()) {
        LOG_DEBUG("query_index_relations: symbol {} not in project_index", symbol_hash);
        co_return serde_raw{"null"};
    }

    // Query each referenced file's MergedIndex for relations of the requested kind.
    std::vector<protocol::Location> locations;

    for(auto file_id: sym_it->second.reference_files) {
        auto file_merged_it = merged_indices.find(file_id);
        if(file_merged_it == merged_indices.end())
            continue;

        auto file_path = project_index.path_pool.path(file_id);
        auto file_uri = lsp::URI::from_file_path(file_path);
        if(!file_uri)
            continue;
        auto file_uri_str = file_uri->str();

        // Use stored content from MergedIndex for offset-to-position conversion.
        auto file_content = file_merged_it->second.content();
        if(file_content.empty())
            continue;

        lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

        file_merged_it->second.lookup(symbol_hash, kind, [&](const index::Relation& r) {
            auto start = file_mapper.to_position(r.range.begin);
            auto end = file_mapper.to_position(r.range.end);
            if(start && end) {
                protocol::Location loc;
                loc.uri = file_uri_str;
                loc.range = protocol::Range{*start, *end};
                locations.push_back(std::move(loc));
            }
            return true;  // continue collecting
        });
    }

    if(locations.empty())
        co_return serde_raw{"null"};

    co_return to_raw(locations);
}

protocol::SymbolKind MasterServer::to_lsp_symbol_kind(SymbolKind kind) {
    switch(kind) {
        case SymbolKind::Namespace: return protocol::SymbolKind::Namespace;
        case SymbolKind::Class: return protocol::SymbolKind::Class;
        case SymbolKind::Struct: return protocol::SymbolKind::Struct;
        case SymbolKind::Union: return protocol::SymbolKind::Class;
        case SymbolKind::Enum: return protocol::SymbolKind::Enum;
        case SymbolKind::Type: return protocol::SymbolKind::TypeParameter;
        case SymbolKind::Field: return protocol::SymbolKind::Field;
        case SymbolKind::EnumMember: return protocol::SymbolKind::EnumMember;
        case SymbolKind::Function: return protocol::SymbolKind::Function;
        case SymbolKind::Method: return protocol::SymbolKind::Method;
        case SymbolKind::Variable: return protocol::SymbolKind::Variable;
        case SymbolKind::Parameter: return protocol::SymbolKind::Variable;
        case SymbolKind::Macro: return protocol::SymbolKind::Function;
        case SymbolKind::Concept: return protocol::SymbolKind::Interface;
        case SymbolKind::Module: return protocol::SymbolKind::Module;
        case SymbolKind::Operator: return protocol::SymbolKind::Operator;
        default: return protocol::SymbolKind::Variable;
    }
}

et::task<std::optional<SymbolInfo>>
    MasterServer::lookup_symbol_at_position(const std::string& uri,
                                            const protocol::Position& position) {
    auto path = uri_to_path(uri);
    auto server_path_id = path_pool.intern(path);

    // Need document text for position-to-offset conversion.
    auto doc_it = documents.find(server_path_id);
    if(doc_it == documents.end())
        co_return std::nullopt;

    lsp::PositionMapper mapper(doc_it->second.text, lsp::PositionEncoding::UTF16);
    auto offset_opt = mapper.to_offset(position);
    if(!offset_opt)
        co_return std::nullopt;
    auto offset = *offset_opt;

    // Find the project-level path_id for this file.
    auto proj_cache_it = project_index.path_pool.find(path);
    if(proj_cache_it == project_index.path_pool.cache.end()) {
        LOG_WARN("lookup_symbol: path '{}' not in project_index (pool has {} entries)",
                 path,
                 project_index.path_pool.paths.size());
        co_return std::nullopt;
    }
    auto proj_path_id = proj_cache_it->second;

    // Lookup occurrence at offset in this file's MergedIndex.
    auto merged_it = merged_indices.find(proj_path_id);
    if(merged_it == merged_indices.end()) {
        LOG_WARN("lookup_symbol: no MergedIndex for proj_path_id={} (have {} shards)",
                 proj_path_id,
                 merged_indices.size());
        co_return std::nullopt;
    }

    index::SymbolHash symbol_hash = 0;
    index::Range occ_range{};
    merged_it->second.lookup(offset, [&](const index::Occurrence& o) {
        symbol_hash = o.target;
        occ_range = o.range;
        return false;  // stop after first match
    });

    if(symbol_hash == 0) {
        LOG_WARN("lookup_symbol: no occurrence at offset {} in '{}'", offset, path);
        co_return std::nullopt;
    }

    // Get symbol info from ProjectIndex.
    auto sym_it = project_index.symbols.find(symbol_hash);
    if(sym_it == project_index.symbols.end())
        co_return std::nullopt;

    // Convert occurrence range to LSP Range.
    auto start = mapper.to_position(occ_range.begin);
    auto end = mapper.to_position(occ_range.end);
    if(!start || !end)
        co_return std::nullopt;

    SymbolInfo info;
    info.hash = symbol_hash;
    info.name = sym_it->second.name;
    info.kind = sym_it->second.kind;
    info.uri = uri;
    info.range = protocol::Range{*start, *end};
    co_return info;
}

std::optional<protocol::Location>
    MasterServer::find_symbol_definition_location(index::SymbolHash hash) {
    auto sym_it = project_index.symbols.find(hash);
    if(sym_it == project_index.symbols.end())
        return std::nullopt;

    // Search reference files for a Definition relation.
    for(auto file_id: sym_it->second.reference_files) {
        auto file_merged_it = merged_indices.find(file_id);
        if(file_merged_it == merged_indices.end())
            continue;

        auto file_path = project_index.path_pool.path(file_id);
        auto file_uri = lsp::URI::from_file_path(file_path);
        if(!file_uri)
            continue;

        // Use stored content from MergedIndex for offset-to-position conversion.
        auto file_content = file_merged_it->second.content();
        if(file_content.empty())
            continue;
        lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

        std::optional<protocol::Location> result;
        file_merged_it->second.lookup(hash,
                                      RelationKind::Definition,
                                      [&](const index::Relation& r) {
                                          auto start = file_mapper.to_position(r.range.begin);
                                          auto end = file_mapper.to_position(r.range.end);
                                          if(start && end) {
                                              protocol::Location loc;
                                              loc.uri = file_uri->str();
                                              loc.range = protocol::Range{*start, *end};
                                              result = std::move(loc);
                                              return false;  // found it, stop
                                          }
                                          return true;
                                      });

        if(result)
            return result;
    }

    return std::nullopt;
}

protocol::CallHierarchyItem MasterServer::build_call_hierarchy_item(const SymbolInfo& info) {
    protocol::CallHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    // Store the symbol hash in data for later use in incoming/outgoing calls.
    item.data = protocol::LSPAny(static_cast<std::int64_t>(info.hash));
    return item;
}

protocol::TypeHierarchyItem MasterServer::build_type_hierarchy_item(const SymbolInfo& info) {
    protocol::TypeHierarchyItem item;
    item.name = info.name;
    item.kind = to_lsp_symbol_kind(info.kind);
    item.uri = info.uri;
    item.range = info.range;
    item.selection_range = info.range;
    item.data = protocol::LSPAny(static_cast<std::int64_t>(info.hash));
    return item;
}

et::task<std::optional<SymbolInfo>>
    MasterServer::resolve_hierarchy_item(const std::string& uri,
                                         const protocol::Range& range,
                                         const std::optional<protocol::LSPAny>& data) {
    // Try to extract symbol hash from the stored data field first.
    if(data) {
        if(auto* int_val = std::get_if<std::int64_t>(&*data)) {
            auto hash = static_cast<index::SymbolHash>(*int_val);
            auto sym_it = project_index.symbols.find(hash);
            if(sym_it != project_index.symbols.end()) {
                SymbolInfo info;
                info.hash = hash;
                info.name = sym_it->second.name;
                info.kind = sym_it->second.kind;
                info.uri = uri;
                info.range = range;
                co_return info;
            }
        }
    }

    // Fallback: re-lookup from position (requires document to be open).
    co_return co_await lookup_symbol_at_position(uri, range.start);
}

void MasterServer::register_handlers() {
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

        // Text document sync: incremental
        protocol::TextDocumentSyncOptions sync_opts;
        sync_opts.open_close = true;
        sync_opts.change = protocol::TextDocumentSyncKind::Incremental;
        sync_opts.save = protocol::variant<protocol::boolean, protocol::SaveOptions>{true};
        result.capabilities.text_document_sync = std::move(sync_opts);

        // Feature capabilities
        result.capabilities.hover_provider = true;
        result.capabilities.completion_provider = protocol::CompletionOptions{};
        result.capabilities.signature_help_provider = protocol::SignatureHelpOptions{};
        result.capabilities.definition_provider = true;
        result.capabilities.references_provider = true;
        result.capabilities.document_symbol_provider = true;
        result.capabilities.document_link_provider = protocol::DocumentLinkOptions{};
        result.capabilities.code_action_provider = true;
        result.capabilities.folding_range_provider = true;
        result.capabilities.inlay_hint_provider = true;
        result.capabilities.call_hierarchy_provider = true;
        result.capabilities.type_hierarchy_provider = true;
        result.capabilities.workspace_symbol_provider = true;

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

        // Persist index state before stopping.
        save_index();

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

        // Apply incremental changes
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

        // Notify the owning stateful worker so it marks the document dirty
        worker::DocumentUpdateParams update;
        update.path = path;
        update.version = doc.version;
        update.text = doc.text;
        pool.notify_stateful(path_id, update);
    });

    // === textDocument/didClose ===
    peer.on_notification([this](const protocol::DidCloseTextDocumentParams& params) {
        if(lifecycle != ServerLifecycle::Ready)
            return;

        auto path = uri_to_path(params.text_document.uri);
        auto path_id = path_pool.intern(path);

        // Cancel in-flight module compilations for this file.
        if(compile_graph && compile_graph->has_unit(path_id)) {
            compile_graph->update(path_id);
        }

        documents.erase(path_id);
        pch_states.erase(path_id);
        ast_deps.erase(path_id);

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

        // Invalidate this file and cascade to dependents in the compile graph.
        if(compile_graph) {
            auto dirtied = compile_graph->update(path_id);
            // Remove stale PCMs for all invalidated units.
            for(auto dirty_id: dirtied) {
                pcm_paths.erase(dirty_id);
            }
            // Mark ast_dirty for open documents that depend on the saved file.
            for(auto dirty_id: dirtied) {
                auto doc_it = documents.find(dirty_id);
                if(doc_it != documents.end()) {
                    doc_it->second.ast_dirty = true;
                }
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

            // Try index-based lookup first.
            auto result = co_await query_index_relations(uri, pos, RelationKind::Definition);
            if(result.has_value() && !result.value().empty() && result.value().data != "null") {
                co_return std::move(result).value();
            }

            // Fall back to stateful worker AST query.
            co_return co_await forward_stateful<worker::GoToDefinitionParams>(uri, pos);
        });

    // --- textDocument/references ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::ReferenceParams& params) -> RawResult {
            auto& uri = params.text_document_position_params.text_document.uri;
            auto& pos = params.text_document_position_params.position;

            auto refs = co_await query_index_relations(uri, pos, RelationKind::Reference);

            if(params.context.include_declaration) {
                // Also include Definition locations when the client requests the declaration.
                auto defs = co_await query_index_relations(uri, pos, RelationKind::Definition);
                if(defs.has_value() && !defs.value().empty() && defs.value().data != "null") {
                    if(!refs.has_value() || refs.value().empty() || refs.value().data == "null") {
                        co_return std::move(defs).value();
                    }
                    // Merge: parse both JSON arrays and concatenate.
                    auto& ref_json = refs.value().data;
                    auto& def_json = defs.value().data;
                    // Both are JSON arrays like "[...]". Merge them.
                    if(ref_json.size() > 2 && def_json.size() > 2) {
                        // Remove trailing ']' from refs, add comma, add def content without leading
                        // '['
                        std::string merged = ref_json.substr(0, ref_json.size() - 1);
                        merged += ',';
                        merged += def_json.substr(1);
                        co_return serde_raw{std::move(merged)};
                    }
                }
            }

            co_return refs;
        });

    // =========================================================================
    // Feature requests routed to stateless workers
    // =========================================================================

    // --- textDocument/completion ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::CompletionParams& params) -> RawResult {
            co_return co_await forward_stateless<worker::CompletionParams>(
                params.text_document_position_params.text_document.uri,
                params.text_document_position_params.position);
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
        auto info = co_await lookup_symbol_at_position(
            params.text_document_position_params.text_document.uri,
            params.text_document_position_params.position);
        if(!info)
            co_return serde_raw{"null"};

        // Only functions/methods are valid for call hierarchy.
        if(!(info->kind == SymbolKind::Function || info->kind == SymbolKind::Method))
            co_return serde_raw{"null"};

        std::vector<protocol::CallHierarchyItem> items;
        items.push_back(build_call_hierarchy_item(*info));
        co_return to_raw(items);
    });

    // --- callHierarchy/incomingCalls ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyIncomingCallsParams& params) -> RawResult {
        // Re-lookup the symbol from the item.
        auto info =
            co_await resolve_hierarchy_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};

        auto sym_it = project_index.symbols.find(info->hash);
        if(sym_it == project_index.symbols.end())
            co_return serde_raw{"null"};

        // Collect all Caller relations across reference files.
        // Caller relation: on the callee symbol, target_symbol is the caller's hash.
        std::vector<protocol::CallHierarchyIncomingCall> results;

        // Group call sites by caller symbol hash.
        llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> caller_ranges;

        for(auto file_id: sym_it->second.reference_files) {
            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            // Use stored content from MergedIndex for offset-to-position conversion.
            auto file_content = file_merged_it->second.content();
            if(file_content.empty())
                continue;

            lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

            file_merged_it->second.lookup(info->hash,
                                          RelationKind::Caller,
                                          [&](const index::Relation& r) {
                                              auto start = file_mapper.to_position(r.range.begin);
                                              auto end = file_mapper.to_position(r.range.end);
                                              if(start && end) {
                                                  caller_ranges[r.target_symbol].push_back(
                                                      protocol::Range{*start, *end});
                                              }
                                              return true;
                                          });
        }

        // Build incoming call items from grouped caller symbols.
        for(auto& [caller_hash, ranges]: caller_ranges) {
            auto def_loc = find_symbol_definition_location(caller_hash);
            auto caller_sym_it = project_index.symbols.find(caller_hash);
            if(caller_sym_it == project_index.symbols.end())
                continue;

            if(!def_loc)
                continue;

            protocol::CallHierarchyItem caller_item;
            caller_item.name = caller_sym_it->second.name;
            caller_item.kind = to_lsp_symbol_kind(caller_sym_it->second.kind);
            caller_item.uri = def_loc->uri;
            caller_item.range = def_loc->range;
            caller_item.selection_range = def_loc->range;
            caller_item.data = protocol::LSPAny(static_cast<std::int64_t>(caller_hash));

            protocol::CallHierarchyIncomingCall call;
            call.from = std::move(caller_item);
            call.from_ranges = std::move(ranges);
            results.push_back(std::move(call));
        }

        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    // --- callHierarchy/outgoingCalls ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::CallHierarchyOutgoingCallsParams& params) -> RawResult {
        auto info =
            co_await resolve_hierarchy_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};

        auto sym_it = project_index.symbols.find(info->hash);
        if(sym_it == project_index.symbols.end())
            co_return serde_raw{"null"};

        // Collect Callee relations (outgoing calls from this function).
        // Group call sites by callee symbol hash.
        llvm::DenseMap<index::SymbolHash, std::vector<protocol::Range>> callee_ranges;

        for(auto file_id: sym_it->second.reference_files) {
            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            // Use stored content from MergedIndex for offset-to-position conversion.
            auto file_content = file_merged_it->second.content();
            if(file_content.empty())
                continue;

            lsp::PositionMapper file_mapper(file_content, lsp::PositionEncoding::UTF16);

            file_merged_it->second.lookup(info->hash,
                                          RelationKind::Callee,
                                          [&](const index::Relation& r) {
                                              auto start = file_mapper.to_position(r.range.begin);
                                              auto end = file_mapper.to_position(r.range.end);
                                              if(start && end) {
                                                  callee_ranges[r.target_symbol].push_back(
                                                      protocol::Range{*start, *end});
                                              }
                                              return true;
                                          });
        }

        std::vector<protocol::CallHierarchyOutgoingCall> results;
        for(auto& [callee_hash, ranges]: callee_ranges) {
            auto def_loc = find_symbol_definition_location(callee_hash);
            auto callee_sym_it = project_index.symbols.find(callee_hash);
            if(callee_sym_it == project_index.symbols.end())
                continue;

            if(!def_loc)
                continue;

            protocol::CallHierarchyItem callee_item;
            callee_item.name = callee_sym_it->second.name;
            callee_item.kind = to_lsp_symbol_kind(callee_sym_it->second.kind);
            callee_item.uri = def_loc->uri;
            callee_item.range = def_loc->range;
            callee_item.selection_range = def_loc->range;
            callee_item.data = protocol::LSPAny(static_cast<std::int64_t>(callee_hash));

            protocol::CallHierarchyOutgoingCall call;
            call.to = std::move(callee_item);
            call.from_ranges = std::move(ranges);
            results.push_back(std::move(call));
        }

        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    // --- textDocument/prepareTypeHierarchy ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchyPrepareParams& params) -> RawResult {
        auto info = co_await lookup_symbol_at_position(
            params.text_document_position_params.text_document.uri,
            params.text_document_position_params.position);
        if(!info)
            co_return serde_raw{"null"};

        // Only class-like types are valid for type hierarchy.
        if(!(info->kind == SymbolKind::Class || info->kind == SymbolKind::Struct ||
             info->kind == SymbolKind::Enum || info->kind == SymbolKind::Union))
            co_return serde_raw{"null"};

        std::vector<protocol::TypeHierarchyItem> items;
        items.push_back(build_type_hierarchy_item(*info));
        co_return to_raw(items);
    });

    // --- typeHierarchy/supertypes ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySupertypesParams& params) -> RawResult {
        auto info =
            co_await resolve_hierarchy_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};

        auto sym_it = project_index.symbols.find(info->hash);
        if(sym_it == project_index.symbols.end())
            co_return serde_raw{"null"};

        // Find Base relations: supertypes of this class.
        std::vector<protocol::TypeHierarchyItem> results;

        for(auto file_id: sym_it->second.reference_files) {
            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            file_merged_it->second.lookup(
                info->hash,
                RelationKind::Base,
                [&](const index::Relation& r) {
                    auto base_hash = r.target_symbol;
                    auto base_sym_it = project_index.symbols.find(base_hash);
                    if(base_sym_it == project_index.symbols.end())
                        return true;

                    // Find the definition location of the base class.
                    auto def_loc = find_symbol_definition_location(base_hash);
                    if(!def_loc)
                        return true;

                    protocol::TypeHierarchyItem item;
                    item.name = base_sym_it->second.name;
                    item.kind = to_lsp_symbol_kind(base_sym_it->second.kind);
                    item.uri = def_loc->uri;
                    item.range = def_loc->range;
                    item.selection_range = def_loc->range;
                    item.data = protocol::LSPAny(static_cast<std::int64_t>(base_hash));
                    results.push_back(std::move(item));
                    return true;
                });
        }

        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    // --- typeHierarchy/subtypes ---
    peer.on_request([this](RequestContext& ctx,
                           const protocol::TypeHierarchySubtypesParams& params) -> RawResult {
        auto info =
            co_await resolve_hierarchy_item(params.item.uri, params.item.range, params.item.data);
        if(!info)
            co_return serde_raw{"null"};

        auto sym_it = project_index.symbols.find(info->hash);
        if(sym_it == project_index.symbols.end())
            co_return serde_raw{"null"};

        // Find Derived relations across all reference files: subtypes of this class.
        std::vector<protocol::TypeHierarchyItem> results;

        for(auto file_id: sym_it->second.reference_files) {
            auto file_merged_it = merged_indices.find(file_id);
            if(file_merged_it == merged_indices.end())
                continue;

            file_merged_it->second.lookup(
                info->hash,
                RelationKind::Derived,
                [&](const index::Relation& r) {
                    auto derived_hash = r.target_symbol;
                    auto derived_sym_it = project_index.symbols.find(derived_hash);
                    if(derived_sym_it == project_index.symbols.end())
                        return true;

                    auto def_loc = find_symbol_definition_location(derived_hash);
                    if(!def_loc)
                        return true;

                    protocol::TypeHierarchyItem item;
                    item.name = derived_sym_it->second.name;
                    item.kind = to_lsp_symbol_kind(derived_sym_it->second.kind);
                    item.uri = def_loc->uri;
                    item.range = def_loc->range;
                    item.selection_range = def_loc->range;
                    item.data = protocol::LSPAny(static_cast<std::int64_t>(derived_hash));
                    results.push_back(std::move(item));
                    return true;
                });
        }

        if(results.empty())
            co_return serde_raw{"null"};
        co_return to_raw(results);
    });

    // --- workspace/symbol ---
    peer.on_request(
        [this](RequestContext& ctx, const protocol::WorkspaceSymbolParams& params) -> RawResult {
            auto query = llvm::StringRef(params.query);
            std::vector<protocol::SymbolInformation> results;

            // Case-insensitive substring match on symbol names.
            std::string query_lower = query.lower();

            for(auto& [hash, symbol]: project_index.symbols) {
                if(results.size() >= 100)
                    break;

                // Skip non-indexable symbol kinds (punctuation, literals, etc.)
                // Skip non-indexable symbol kinds using a positive check instead.
                auto sk = symbol.kind;
                if(!(sk == SymbolKind::Namespace || sk == SymbolKind::Class ||
                     sk == SymbolKind::Struct || sk == SymbolKind::Union ||
                     sk == SymbolKind::Enum || sk == SymbolKind::Type || sk == SymbolKind::Field ||
                     sk == SymbolKind::EnumMember || sk == SymbolKind::Function ||
                     sk == SymbolKind::Method || sk == SymbolKind::Variable ||
                     sk == SymbolKind::Parameter || sk == SymbolKind::Macro ||
                     sk == SymbolKind::Concept || sk == SymbolKind::Module ||
                     sk == SymbolKind::Operator || sk == SymbolKind::MacroParameter ||
                     sk == SymbolKind::Label || sk == SymbolKind::Attribute))
                    continue;

                if(symbol.name.empty())
                    continue;

                // Check case-insensitive substring match.
                std::string name_lower = llvm::StringRef(symbol.name).lower();
                if(!query_lower.empty() && name_lower.find(query_lower) == std::string::npos)
                    continue;

                auto def_loc = find_symbol_definition_location(hash);
                if(!def_loc)
                    continue;

                protocol::SymbolInformation info;
                info.name = symbol.name;
                info.kind = to_lsp_symbol_kind(symbol.kind);
                info.location = std::move(*def_loc);
                results.push_back(std::move(info));
            }

            if(results.empty())
                co_return serde_raw{"null"};
            co_return to_raw(results);
        });
}

}  // namespace clice
