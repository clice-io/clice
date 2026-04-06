#include "server/master_server.h"

#include <algorithm>
#include <chrono>
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

/// Serializable cache structures for cache.json persistence.
/// Paths are stored in a shared table and referenced by index to avoid
/// redundant storage (a single file can depend on thousands of headers,
/// many of which are shared across entries).
namespace {

struct CacheDepEntry {
    std::uint32_t path;  // index into CacheData::paths
    std::uint64_t hash;
};

struct CachePCHEntry {
    std::string filename;
    std::uint32_t source_file;  // index into CacheData::paths
    std::uint64_t hash;
    std::uint32_t bound;
    std::int64_t build_at;
    std::vector<CacheDepEntry> deps;
};

struct CachePCMEntry {
    std::string filename;
    std::uint32_t source_file;  // index into CacheData::paths
    std::string module_name;
    std::int64_t build_at;
    std::vector<CacheDepEntry> deps;
};

struct CacheData {
    std::vector<std::string> paths;
    std::vector<CachePCHEntry> pch;
    std::vector<CachePCMEntry> pcm;
};

}  // namespace

void MasterServer::load_cache() {
    if(config.cache_dir.empty())
        return;

    auto cache_path = path::join(config.cache_dir, "cache", "cache.json");
    auto content = fs::read(cache_path);
    if(!content) {
        LOG_DEBUG("No cache.json found at {}", cache_path);
        return;
    }

    CacheData data;
    auto status = et::serde::json::from_json(*content, data);
    if(!status) {
        LOG_WARN("Failed to parse cache.json");
        return;
    }

    auto resolve = [&](std::uint32_t idx) -> llvm::StringRef {
        return idx < data.paths.size() ? llvm::StringRef(data.paths[idx]) : "";
    };

    for(auto& entry: data.pch) {
        auto pch_path = path::join(config.cache_dir, "cache", "pch", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pch_path) || source.empty())
            continue;

        DepsSnapshot deps;
        deps.build_at = entry.build_at;
        for(auto& dep: entry.deps) {
            auto dep_path = resolve(dep.path);
            if(dep_path.empty())
                continue;
            deps.path_ids.push_back(path_pool.intern(dep_path));
            deps.hashes.push_back(dep.hash);
        }

        auto path_id = path_pool.intern(source);
        auto& st = pch_states[path_id];
        st.path = pch_path;
        st.hash = entry.hash;
        st.bound = entry.bound;
        st.deps = std::move(deps);

        LOG_DEBUG("Loaded cached PCH: {} -> {}", source, pch_path);
    }

    for(auto& entry: data.pcm) {
        auto pcm_path = path::join(config.cache_dir, "cache", "pcm", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pcm_path) || source.empty())
            continue;

        DepsSnapshot deps;
        deps.build_at = entry.build_at;
        for(auto& dep: entry.deps) {
            auto dep_path = resolve(dep.path);
            if(dep_path.empty())
                continue;
            deps.path_ids.push_back(path_pool.intern(dep_path));
            deps.hashes.push_back(dep.hash);
        }

        auto path_id = path_pool.intern(source);
        pcm_states[path_id] = {pcm_path, std::move(deps)};
        pcm_paths[path_id] = pcm_path;

        LOG_DEBUG("Loaded cached PCM: {} (module {}) -> {}", source, entry.module_name, pcm_path);
    }

    LOG_INFO("Loaded cache.json: {} PCH entries, {} PCM entries",
             pch_states.size(),
             pcm_states.size());
}

void MasterServer::save_cache() {
    if(config.cache_dir.empty())
        return;

    CacheData data;
    std::unordered_map<std::string, std::uint32_t> index_map;

    auto intern = [&](std::uint32_t runtime_path_id) -> std::uint32_t {
        auto path = std::string(path_pool.resolve(runtime_path_id));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    for(auto& [path_id, st]: pch_states) {
        if(st.path.empty())
            continue;

        CachePCHEntry entry;
        entry.filename = std::string(path::filename(st.path));
        entry.source_file = intern(path_id);
        entry.hash = st.hash;
        entry.bound = st.bound;
        entry.build_at = st.deps.build_at;
        for(std::size_t i = 0; i < st.deps.path_ids.size(); ++i) {
            entry.deps.push_back({intern(st.deps.path_ids[i]), st.deps.hashes[i]});
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: pcm_states) {
        if(st.path.empty())
            continue;

        CachePCMEntry entry;
        entry.filename = std::string(path::filename(st.path));
        entry.source_file = intern(path_id);
        auto mod_it = path_to_module.find(path_id);
        entry.module_name = mod_it != path_to_module.end() ? mod_it->second : "";
        entry.build_at = st.deps.build_at;
        for(std::size_t i = 0; i < st.deps.path_ids.size(); ++i) {
            entry.deps.push_back({intern(st.deps.path_ids[i]), st.deps.hashes[i]});
        }
        data.pcm.push_back(std::move(entry));
    }

    auto json_str = et::serde::json::to_json(data);
    if(!json_str) {
        LOG_WARN("Failed to serialize cache.json");
        return;
    }

    auto cache_path = path::join(config.cache_dir, "cache", "cache.json");
    auto tmp_path = cache_path + ".tmp";
    auto write_result = fs::write(tmp_path, *json_str);
    if(!write_result) {
        LOG_WARN("Failed to write cache.json.tmp: {}", write_result.error().message());
        return;
    }
    auto rename_result = fs::rename(tmp_path, cache_path);
    if(!rename_result) {
        LOG_WARN("Failed to rename cache.json.tmp to cache.json: {}",
                 rename_result.error().message());
    }
}

void MasterServer::cleanup_cache(int max_age_days) {
    if(config.cache_dir.empty())
        return;

    auto now = std::chrono::system_clock::now();
    auto max_age = std::chrono::hours(max_age_days * 24);

    for(auto* subdir: {"cache/pch", "cache/pcm"}) {
        auto dir = path::join(config.cache_dir, subdir);
        std::error_code ec;
        for(auto it = llvm::sys::fs::directory_iterator(dir, ec);
            !ec && it != llvm::sys::fs::directory_iterator();
            it.increment(ec)) {
            llvm::sys::fs::file_status status;
            if(auto stat_ec = llvm::sys::fs::status(it->path(), status))
                continue;

            auto mtime = status.getLastModificationTime();
            auto age = now - mtime;
            if(age > max_age) {
                llvm::sys::fs::remove(it->path());
                LOG_DEBUG("Cleaned up stale cache file: {}", it->path());
            }
        }
    }
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

        // Create cache/pch/ and cache/pcm/ subdirectories
        for(auto* subdir: {"cache/pch", "cache/pcm"}) {
            auto dir = path::join(config.cache_dir, subdir);
            auto ec2 = llvm::sys::fs::create_directories(dir);
            if(ec2) {
                LOG_WARN("Failed to create {}: {}", dir, ec2.message());
            }
        }

        // Clean up stale files first, then load — load_cache() only restores
        // entries still listed in cache.json, so cleanup won't delete live files.
        cleanup_cache();
        load_cache();
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

    // Build reverse include map so headers can find their host source files.
    dependency_graph.build_reverse_map();

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

        // Compute deterministic content-addressed PCM path.
        // Replace ':' with '-' in module name for filesystem safety.
        // Hash includes file path AND compile arguments so that argument
        // changes (e.g. -DFOO) invalidate the cached PCM.
        auto safe_module_name = mod_it->second;
        std::ranges::replace(safe_module_name, ':', '-');
        std::string hash_input = file_path;
        for(auto& arg: pcm_params.arguments) {
            hash_input += arg;
        }
        auto args_hash = llvm::xxh3_64bits(llvm::StringRef(hash_input));
        auto pcm_filename = std::format("{}-{:016x}.pcm", safe_module_name, args_hash);
        auto pcm_path = path::join(config.cache_dir, "cache", "pcm", pcm_filename);

        // Check if cached PCM is still valid.
        if(auto pcm_it = pcm_states.find(path_id); pcm_it != pcm_states.end()) {
            if(!pcm_it->second.path.empty() && llvm::sys::fs::exists(pcm_it->second.path) &&
               !deps_changed(path_pool, pcm_it->second.deps)) {
                pcm_paths[path_id] = pcm_it->second.path;
                co_return true;
            }
        }

        pcm_params.module_name = mod_it->second;
        pcm_params.output_path = pcm_path;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        for(auto& [pid, existing_pcm_path]: pcm_paths) {
            auto dep_mod_it = path_to_module.find(pid);
            if(dep_mod_it != path_to_module.end()) {
                pcm_params.pcms[dep_mod_it->second] = existing_pcm_path;
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
        pcm_states[path_id] = {result.value().pcm_path,
                               capture_deps_snapshot(path_pool, result.value().deps)};
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, result.value().pcm_path);

        // Persist cache metadata after successful build.
        save_cache();

        co_return true;
    };

    compile_graph = std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", path_to_module.size());
}

std::optional<HeaderFileContext>
    MasterServer::resolve_header_context(std::uint32_t header_path_id) {
    // Find source files that transitively include this header.
    auto hosts = dependency_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    // If there's an active context override, prefer that host.
    std::uint32_t host_path_id = 0;
    std::vector<std::uint32_t> chain;
    auto active_it = active_contexts.find(header_path_id);
    if(active_it != active_contexts.end()) {
        auto preferred = active_it->second;
        auto preferred_path = path_pool.resolve(preferred);
        auto results = cdb.lookup(preferred_path, {.suppress_logging = true});
        if(!results.empty()) {
            auto c = dependency_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                chain = std::move(c);
            }
        }
    }

    // Fall back to the first available host that has a CDB entry.
    if(chain.empty()) {
        for(auto candidate: hosts) {
            auto candidate_path = path_pool.resolve(candidate);
            auto results = cdb.lookup(candidate_path, {.suppress_logging = true});
            if(results.empty())
                continue;
            auto c = dependency_graph.find_include_chain(candidate, header_path_id);
            if(c.empty())
                continue;
            host_path_id = candidate;
            chain = std::move(c);
            break;
        }
    }

    if(chain.empty()) {
        LOG_DEBUG("resolve_header_context: no usable host with include chain for path_id={}",
                  header_path_id);
        return std::nullopt;
    }

    // Build preamble text: for each file in the chain except the last (target),
    // append all content up to (but not including) the line that includes the
    // next file in the chain.
    std::string preamble;
    for(std::size_t i = 0; i + 1 < chain.size(); ++i) {
        auto cur_id = chain[i];
        auto next_id = chain[i + 1];

        auto cur_path = path_pool.resolve(cur_id);
        auto next_path = path_pool.resolve(next_id);
        auto next_filename = llvm::sys::path::filename(next_path);

        // Prefer in-memory document text over disk content.
        std::string content;
        if(auto doc_it = documents.find(cur_id); doc_it != documents.end()) {
            content = doc_it->second.text;
        } else {
            auto buf = llvm::MemoryBuffer::getFile(cur_path);
            if(!buf) {
                LOG_WARN("resolve_header_context: cannot read {}", cur_path);
                return std::nullopt;
            }
            content = (*buf)->getBuffer().str();
        }

        // Scan line by line for the #include that brings in next_filename.
        llvm::StringRef content_ref(content);
        std::size_t line_start = 0;
        std::size_t include_line_start = std::string::npos;
        while(line_start <= content_ref.size()) {
            auto newline_pos = content_ref.find('\n', line_start);
            auto line_end =
                (newline_pos == llvm::StringRef::npos) ? content_ref.size() : newline_pos;
            auto line = content_ref.slice(line_start, line_end).trim();

            if(line.starts_with("#include") || line.starts_with("# include")) {
                // Check if this line references the next file in the chain.
                if(line.contains(next_filename)) {
                    include_line_start = line_start;
                    break;
                }
            }

            line_start =
                (newline_pos == llvm::StringRef::npos) ? content_ref.size() + 1 : newline_pos + 1;
        }

        // Emit a #line marker then all content before the include line.
        preamble += std::format("#line 1 \"{}\"\n", cur_path.str());
        if(include_line_start != std::string::npos) {
            preamble += content_ref.substr(0, include_line_start).str();
        } else {
            // No matching include line found — emit the whole file to be safe.
            LOG_DEBUG("resolve_header_context: include line for {} not found in {}, emitting full",
                      next_filename,
                      cur_path);
            preamble += content;
        }
    }

    // Hash the preamble and write to cache directory.
    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(preamble));
    auto preamble_filename = std::format("{:016x}.h", preamble_hash);
    auto preamble_dir = path::join(config.cache_dir, "header_context");
    auto preamble_path = path::join(preamble_dir, preamble_filename);

    if(!llvm::sys::fs::exists(preamble_path)) {
        auto ec = llvm::sys::fs::create_directories(preamble_dir);
        if(ec) {
            LOG_WARN("resolve_header_context: cannot create dir {}: {}",
                     preamble_dir,
                     ec.message());
            return std::nullopt;
        }
        if(auto result = fs::write(preamble_path, preamble); !result) {
            LOG_WARN("resolve_header_context: cannot write preamble {}: {}",
                     preamble_path,
                     result.error().message());
            return std::nullopt;
        }
        LOG_INFO("resolve_header_context: wrote preamble {} for header path_id={}",
                 preamble_path,
                 header_path_id);
    }

    return HeaderFileContext{host_path_id, preamble_path, preamble_hash};
}

bool MasterServer::fill_compile_args(llvm::StringRef path,
                                     std::string& directory,
                                     std::vector<std::string>& arguments) {
    auto results = cdb.lookup(path, {.query_toolchain = true});
    if(!results.empty()) {
        auto& ctx = results.front();
        directory = ctx.directory.str();
        arguments.clear();
        for(auto* arg: ctx.arguments) {
            arguments.emplace_back(arg);
        }
        return true;
    }

    // No direct CDB entry — try to compile the header in context of a host source.
    auto path_id = path_pool.intern(path);

    // Use cached context if available; otherwise resolve.
    // If an active context override exists, invalidate cache if it points to
    // a different host so we re-resolve with the correct one.
    const HeaderFileContext* ctx_ptr = nullptr;
    auto ctx_it = header_file_contexts.find(path_id);
    auto active_it = active_contexts.find(path_id);
    if(ctx_it != header_file_contexts.end()) {
        // Check if the cached context matches the active context override.
        if(active_it != active_contexts.end() && ctx_it->second.host_path_id != active_it->second) {
            header_file_contexts.erase(ctx_it);
            ctx_it = header_file_contexts.end();
        } else {
            ctx_ptr = &ctx_it->second;
        }
    }
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return false;
        }
        header_file_contexts[path_id] = std::move(*resolved);
        ctx_ptr = &header_file_contexts[path_id];
    }

    auto host_path = path_pool.resolve(ctx_ptr->host_path_id);
    auto host_results = cdb.lookup(host_path, {.query_toolchain = true});
    if(host_results.empty()) {
        LOG_WARN("fill_compile_args: host {} has no CDB entry", host_path);
        return false;
    }

    auto& host_ctx = host_results.front();
    directory = host_ctx.directory.str();
    arguments.clear();

    // Copy host arguments, replacing the host source file path with the header
    // file path.  We identify the host source by matching against the CDB file
    // entry rather than guessing based on positional order, so that entries like
    // "clang++ -c main.cpp -o main.o" are handled correctly.
    for(auto& arg: host_ctx.arguments) {
        if(llvm::StringRef(arg) == host_path) {
            arguments.emplace_back(path);
        } else {
            arguments.emplace_back(arg);
        }
    }

    // Inject the preamble so the compiler sees all context code that normally
    // precedes this header in the host translation unit.
    // For cc1 args (["clang++", "-cc1", ...]), insert after "-cc1" at position 2.
    // For driver args, insert after the driver binary at position 1.
    std::size_t inject_pos = 1;
    if(arguments.size() >= 2 && arguments[1] == "-cc1") {
        inject_pos = 2;
    }
    arguments.insert(arguments.begin() + inject_pos, ctx_ptr->preamble_path);
    arguments.insert(arguments.begin() + inject_pos, "-include");

    LOG_INFO("fill_compile_args: using header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
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
        pch_states.erase(path_id);
        co_return true;
    }

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(text).substr(0, bound));

    // Deterministic content-addressed PCH path.
    auto pch_path =
        path::join(config.cache_dir, "cache", "pch", std::format("{:016x}.pch", preamble_hash));

    // Reuse existing PCH if preamble content and deps haven't changed.
    if(auto it = pch_states.find(path_id); it != pch_states.end()) {
        auto& st = it->second;
        if(st.hash == preamble_hash && !st.path.empty() && !deps_changed(path_pool, st.deps)) {
            st.bound = bound;
            co_return true;
        }
    }

    // Preamble incomplete (user still typing) — defer rebuild, reuse old PCH if available.
    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        co_return pch_states.count(path_id) && !pch_states[path_id].path.empty();
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
    pch_params.output_path = pch_path;

    LOG_DEBUG("Building PCH for {}, bound={}, output={}", path, bound, pch_path);

    auto result = co_await pool.send_stateless(pch_params);

    if(!result.has_value() || !result.value().success) {
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        pch_states[path_id].building.reset();
        completion->set();
        co_return false;
    }

    // Update state — no need to delete old file; content-addressed names differ
    // when content differs, and the 7-day cleanup handles orphaned files.
    auto& st = pch_states[path_id];
    st.path = result.value().pch_path;
    st.bound = bound;
    st.hash = preamble_hash;
    st.deps = capture_deps_snapshot(path_pool, result.value().deps);
    st.building.reset();

    LOG_INFO("PCH built for {}: {}", path, result.value().pch_path);

    // Persist cache metadata after successful build.
    save_cache();

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

    // Scan buffer text for module imports that might not be in compile_graph yet.
    // When a user adds `import std;` without saving, the compile_graph (disk-based)
    // doesn't know about the new dependency. Scan the in-memory text to find them.
    {
        auto scan_result = scan(text);
        for(auto& mod_name: scan_result.modules) {
            if(mod_name.empty()) {
                continue;
            }
            bool found = false;
            for(auto& [pid, name]: path_to_module) {
                if(name == mod_name) {
                    // If PCM not already built, try to build it.
                    if(pcm_paths.find(pid) == pcm_paths.end()) {
                        if(compile_graph && compile_graph->has_unit(pid)) {
                            co_await compile_graph->compile_deps(pid);
                        }
                    }
                    found = true;
                    break;
                }
            }
            if(!found) {
                LOG_DEBUG("Buffer imports unknown module '{}', skipping", mod_name);
            }
        }
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
        if(!self->fill_compile_args(self->path_pool.resolve(pid),
                                    params.directory,
                                    params.arguments)) {
            finish_compile();
            co_return;
        }

        if(!co_await self->ensure_deps(pid,
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
        self->ast_deps[pid] = capture_deps_snapshot(self->path_pool, result.value().deps);
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
    if(!fill_compile_args(path, directory, arguments))
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

    for(auto& [path_id, module_name]: path_to_module) {
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
    if(!fill_compile_args(path, wp.directory, wp.arguments)) {
        LOG_DEBUG("forward_stateless: no CDB for {}", path);
        co_return serde_raw{};
    }

    // Ensure module deps, PCH, and PCM paths are ready for stateless compilation.
    if(!co_await ensure_deps(path_id, path, wp.text, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
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
        save_index();
        save_cache();

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
                pcm_states.erase(dirty_id);
            }
            // Mark ast_dirty for open documents that depend on the saved file.
            for(auto dirty_id: dirtied) {
                auto doc_it = documents.find(dirty_id);
                if(doc_it != documents.end()) {
                    doc_it->second.ast_dirty = true;
                }
            }
        }

        // Invalidate header contexts whose host is the saved file.
        // Collect entries to erase to avoid modifying the map while iterating.
        llvm::SmallVector<std::uint32_t, 4> stale_headers;
        for(auto& [hdr_id, hdr_ctx]: header_file_contexts) {
            if(hdr_ctx.host_path_id == path_id)
                stale_headers.push_back(hdr_id);
        }
        for(auto hdr_id: stale_headers) {
            header_file_contexts.erase(hdr_id);
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

    // === clice/ Extension Commands ===

    // --- clice/queryContext ---
    peer.on_request(
        "clice/queryContext",
        [this](RequestContext& ctx, const ext::QueryContextParams& params) -> RawResult {
            auto path = uri_to_path(params.uri);
            auto path_id = path_pool.intern(path);
            int offset_val = params.offset.value_or(0);
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

            auto it = active_contexts.find(path_id);
            if(it != active_contexts.end()) {
                auto ctx_path = path_pool.resolve(it->second);
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

            // Set active context and invalidate cached header context so
            // resolve_header_context will pick the new host on next compile.
            active_contexts[path_id] = context_path_id;
            header_file_contexts.erase(path_id);

            // Also invalidate the PCH and AST deps for the old context so
            // they get rebuilt with the new host's preamble.
            pch_states.erase(path_id);
            ast_deps.erase(path_id);

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
