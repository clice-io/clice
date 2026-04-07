#include "server/compiler.h"

#include <chrono>
#include <format>
#include <ranges>
#include <string>
#include <type_traits>
#include <variant>

#include "command/search_config.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/uri.h"
#include "eventide/serde/json/json.h"
#include "index/tu_index.h"
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

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
    std::uint32_t source_file;
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

namespace lsp = eventide::ipc::lsp;
using serde_raw = et::serde::RawValue;

Compiler::Compiler(et::event_loop& loop,
                   et::ipc::JsonPeer& peer,
                   Workspace& workspace,
                   WorkerPool& pool,
                   llvm::DenseMap<std::uint32_t, Session>& sessions) :
    loop(loop), peer(peer), workspace(workspace), pool(pool), sessions(sessions) {}

Compiler::~Compiler() {
    cancel_all();
}

void Compiler::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                             std::uint32_t exclude_path_id) const {
    for(auto& [pid, pcm_path]: workspace.pcm_paths) {
        if(pid == exclude_path_id)
            continue;
        auto mod_it = workspace.path_to_module.find(pid);
        if(mod_it != workspace.path_to_module.end()) {
            pcms[mod_it->second] = pcm_path;
        }
    }
}

void Compiler::cancel_all() {
    if(workspace.compile_graph) {
        workspace.compile_graph->cancel_all();
    }
}

void Compiler::load_cache() {
    if(workspace.config.cache_dir.empty())
        return;

    auto cache_path = path::join(workspace.config.cache_dir, "cache", "cache.json");
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

    auto load_deps = [&](std::int64_t build_at, const auto& dep_entries) -> DepsSnapshot {
        DepsSnapshot deps;
        deps.build_at = build_at;
        for(auto& dep: dep_entries) {
            auto dep_path = resolve(dep.path);
            if(dep_path.empty())
                continue;
            deps.path_ids.push_back(workspace.path_pool.intern(dep_path));
            deps.hashes.push_back(dep.hash);
        }
        return deps;
    };

    for(auto& entry: data.pch) {
        auto pch_path = path::join(workspace.config.cache_dir, "cache", "pch", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pch_path) || source.empty())
            continue;

        auto path_id = workspace.path_pool.intern(source);
        auto& st = workspace.pch_cache[path_id];
        st.path = pch_path;
        st.hash = entry.hash;
        st.bound = entry.bound;
        st.deps = load_deps(entry.build_at, entry.deps);

        LOG_DEBUG("Loaded cached PCH: {} -> {}", source, pch_path);
    }

    for(auto& entry: data.pcm) {
        auto pcm_path = path::join(workspace.config.cache_dir, "cache", "pcm", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pcm_path) || source.empty())
            continue;

        auto path_id = workspace.path_pool.intern(source);
        workspace.pcm_cache[path_id] = {pcm_path, load_deps(entry.build_at, entry.deps)};
        workspace.pcm_paths[path_id] = pcm_path;

        LOG_DEBUG("Loaded cached PCM: {} (module {}) -> {}", source, entry.module_name, pcm_path);
    }

    LOG_INFO("Loaded cache.json: {} PCH entries, {} PCM entries",
             workspace.pch_cache.size(),
             workspace.pcm_cache.size());
}

void Compiler::save_cache() {
    if(workspace.config.cache_dir.empty())
        return;

    CacheData data;
    std::unordered_map<std::string, std::uint32_t> index_map;

    auto intern = [&](std::uint32_t runtime_path_id) -> std::uint32_t {
        auto path = std::string(workspace.path_pool.resolve(runtime_path_id));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    for(auto& [path_id, st]: workspace.pch_cache) {
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

    for(auto& [path_id, st]: workspace.pcm_cache) {
        if(st.path.empty())
            continue;

        CachePCMEntry entry;
        entry.filename = std::string(path::filename(st.path));
        entry.source_file = intern(path_id);
        auto mod_it = workspace.path_to_module.find(path_id);
        entry.module_name = mod_it != workspace.path_to_module.end() ? mod_it->second : "";
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

    auto cache_path = path::join(workspace.config.cache_dir, "cache", "cache.json");
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

void Compiler::cleanup_cache(int max_age_days) {
    if(workspace.config.cache_dir.empty())
        return;

    auto now = std::chrono::system_clock::now();
    auto max_age = std::chrono::hours(max_age_days * 24);

    for(auto* subdir: {"cache/pch", "cache/pcm"}) {
        auto dir = path::join(workspace.config.cache_dir, subdir);
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

void Compiler::build_module_map() {
    for(auto& [module_name, path_ids]: workspace.dep_graph.modules()) {
        for(auto path_id: path_ids) {
            workspace.path_to_module[path_id] = module_name.str();
        }
    }
}

void Compiler::init_compile_graph() {
    if(workspace.path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        return;
    }

    // Lazy dependency resolver: scans a module file on demand to discover imports.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = workspace.path_pool.resolve(path_id);
        auto results =
            workspace.cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty())
            return {};

        auto& ctx = results[0];
        auto scan_result = scan_precise(ctx.arguments, ctx.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = workspace.dep_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        // Module implementation units implicitly depend on their interface unit.
        if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
            auto mod_ids = workspace.dep_graph.lookup_module(scan_result.module_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        return deps;
    };

    // Dispatch: sends BuildPCM request to a stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> et::task<bool> {
        auto mod_it = workspace.path_to_module.find(path_id);
        if(mod_it == workspace.path_to_module.end())
            co_return false;

        auto file_path = std::string(workspace.path_pool.resolve(path_id));

        worker::BuildParams bp;
        bp.kind = worker::BuildKind::BuildPCM;
        bp.file = file_path;
        if(!fill_compile_args(file_path, bp.directory, bp.arguments))
            co_return false;

        // Compute deterministic content-addressed PCM path.
        auto safe_module_name = mod_it->second;
        std::ranges::replace(safe_module_name, ':', '-');
        std::string hash_input = file_path;
        for(auto& arg: bp.arguments) {
            hash_input += arg;
        }
        auto args_hash = llvm::xxh3_64bits(llvm::StringRef(hash_input));
        auto pcm_filename = std::format("{}-{:016x}.pcm", safe_module_name, args_hash);
        auto pcm_path = path::join(workspace.config.cache_dir, "cache", "pcm", pcm_filename);

        // Check if cached PCM is still valid.
        if(auto pcm_it = workspace.pcm_cache.find(path_id); pcm_it != workspace.pcm_cache.end()) {
            if(!pcm_it->second.path.empty() && llvm::sys::fs::exists(pcm_it->second.path) &&
               !deps_changed(workspace.path_pool, pcm_it->second.deps)) {
                workspace.pcm_paths[path_id] = pcm_it->second.path;
                co_return true;
            }
        }

        bp.module_name = mod_it->second;
        bp.output_path = pcm_path;

        // Clang needs ALL transitive PCM deps, not just direct imports.
        fill_pcm_deps(bp.pcms);

        auto result = co_await pool.send_stateless(bp);
        if(!result.has_value() || !result.value().success) {
            LOG_WARN("BuildPCM failed for module {}: {}",
                     mod_it->second,
                     result.has_value() ? result.value().error : result.error().message);
            co_return false;
        }

        workspace.pcm_paths[path_id] = result.value().output_path;
        workspace.pcm_cache[path_id] = {
            result.value().output_path,
            capture_deps_snapshot(workspace.path_pool, result.value().deps)};
        LOG_INFO("Built PCM for module {}: {}", mod_it->second, result.value().output_path);

        // Persist cache metadata after successful build.
        save_cache();

        // Signal that new index data is available for background merge.
        if(on_indexing_needed)
            on_indexing_needed();

        co_return true;
    };

    workspace.compile_graph =
        std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", workspace.path_to_module.size());
}

bool Compiler::fill_compile_args(llvm::StringRef path,
                                 std::string& directory,
                                 std::vector<std::string>& arguments,
                                 Session* session) {
    auto path_id = workspace.path_pool.intern(path);

    // 1. If the session has an active header context via switchContext,
    //    use the host source's CDB entry with file path replaced and preamble injected.
    if(session && session->active_context.has_value()) {
        return fill_header_context_args(path, path_id, directory, arguments, session);
    }

    // 2. Normal CDB lookup for the file itself.
    auto results = workspace.cdb.lookup(path, {.query_toolchain = true});
    if(!results.empty()) {
        auto& ctx = results.front();
        directory = ctx.directory.str();
        arguments.clear();
        for(auto* arg: ctx.arguments) {
            arguments.emplace_back(arg);
        }
        return true;
    }

    // 3. No CDB entry — try automatic header context resolution.
    return fill_header_context_args(path, path_id, directory, arguments, session);
}

bool Compiler::fill_header_context_args(llvm::StringRef path,
                                        std::uint32_t path_id,
                                        std::string& directory,
                                        std::vector<std::string>& arguments,
                                        Session* session) {
    // Use cached context if available; otherwise resolve.
    // If an active context override exists, invalidate cache if it points to
    // a different host so we re-resolve with the correct one.
    const HeaderFileContext* ctx_ptr = nullptr;
    if(session && session->header_context.has_value()) {
        if(session->active_context.has_value() &&
           session->header_context->host_path_id != *session->active_context) {
            session->header_context.reset();
        } else {
            ctx_ptr = &*session->header_context;
        }
    }
    if(!ctx_ptr) {
        auto resolved = resolve_header_context(path_id, session);
        if(!resolved) {
            LOG_WARN("No CDB entry and no header context for {}", path);
            return false;
        }
        if(session) {
            session->header_context = std::move(*resolved);
            ctx_ptr = &*session->header_context;
        } else {
            // Background indexing path — no session to store on.
            // Use a temporary (caller will use it immediately).
            // Store in a local and return.
            static thread_local std::optional<HeaderFileContext> tl_ctx;
            tl_ctx = std::move(*resolved);
            ctx_ptr = &*tl_ctx;
        }
    }

    auto host_path = workspace.path_pool.resolve(ctx_ptr->host_path_id);
    auto host_results = workspace.cdb.lookup(host_path, {.query_toolchain = true});
    if(host_results.empty()) {
        LOG_WARN("fill_header_context_args: host {} has no CDB entry", host_path);
        return false;
    }

    auto& host_ctx = host_results.front();
    directory = host_ctx.directory.str();
    arguments.clear();

    // Copy host arguments, replacing the host source file path with the header.
    bool replaced = false;
    for(auto& arg: host_ctx.arguments) {
        if(llvm::StringRef(arg) == host_path) {
            arguments.emplace_back(path);
            replaced = true;
        } else {
            arguments.emplace_back(arg);
        }
    }
    if(!replaced) {
        LOG_WARN("fill_header_context_args: host path {} not found in arguments, appending header",
                 host_path);
        arguments.emplace_back(path);
    }

    // Inject preamble: for cc1 args insert after "-cc1", otherwise after driver.
    std::size_t inject_pos = 1;
    if(arguments.size() >= 2 && arguments[1] == "-cc1") {
        inject_pos = 2;
    }
    arguments.insert(arguments.begin() + inject_pos, ctx_ptr->preamble_path);
    arguments.insert(arguments.begin() + inject_pos, "-include");

    LOG_INFO("fill_compile_args: header context for {} (host={}, preamble={})",
             path,
             host_path,
             ctx_ptr->preamble_path);
    return true;
}

std::optional<HeaderFileContext> Compiler::resolve_header_context(std::uint32_t header_path_id,
                                                                  Session* session) {
    // Find source files that transitively include this header.
    auto hosts = workspace.dep_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    // If there's an active context override, prefer that host.
    std::uint32_t host_path_id = 0;
    std::vector<std::uint32_t> chain;
    if(session && session->active_context.has_value()) {
        auto preferred = *session->active_context;
        auto preferred_path = workspace.path_pool.resolve(preferred);
        auto results = workspace.cdb.lookup(preferred_path, {.suppress_logging = true});
        if(!results.empty()) {
            auto c = workspace.dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                chain = std::move(c);
            }
        }
    }

    // Fall back to the first available host that has a CDB entry.
    if(chain.empty()) {
        for(auto candidate: hosts) {
            auto candidate_path = workspace.path_pool.resolve(candidate);
            auto results = workspace.cdb.lookup(candidate_path, {.suppress_logging = true});
            if(results.empty())
                continue;
            auto c = workspace.dep_graph.find_include_chain(candidate, header_path_id);
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

        auto cur_path = workspace.path_pool.resolve(cur_id);
        auto next_path = workspace.path_pool.resolve(next_id);
        auto next_filename = llvm::sys::path::filename(next_path);

        // Prefer in-memory document text over disk content.
        // Use the session if this file matches the session's path, otherwise
        // fall back to disk.
        std::string content;
        // Note: we don't have the sessions map here, so we always read from disk
        // for intermediate chain files.  The session parameter only covers the
        // header file itself (the target), not intermediate files in the chain.
        auto buf = llvm::MemoryBuffer::getFile(cur_path);
        if(!buf) {
            LOG_WARN("resolve_header_context: cannot read {}", cur_path);
            return std::nullopt;
        }
        content = (*buf)->getBuffer().str();

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
                // Extract the filename from the #include directive.
                // Handles: #include "foo.h", #include <foo.h>, # include "foo.h"
                auto quote_start = line.find_first_of("\"<");
                auto quote_end = llvm::StringRef::npos;
                if(quote_start != llvm::StringRef::npos) {
                    char close = (line[quote_start] == '"') ? '"' : '>';
                    quote_end = line.find(close, quote_start + 1);
                }
                if(quote_start != llvm::StringRef::npos && quote_end != llvm::StringRef::npos) {
                    auto included = line.slice(quote_start + 1, quote_end);
                    auto included_filename = llvm::sys::path::filename(included);
                    if(included_filename == next_filename) {
                        include_line_start = line_start;
                        break;
                    }
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
    auto preamble_dir = path::join(workspace.config.cache_dir, "header_context");
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

std::string Compiler::uri_to_path(const std::string& uri) {
    auto parsed = lsp::URI::parse(uri);
    if(parsed.has_value()) {
        auto path = parsed->file_path();
        if(path.has_value()) {
            return std::move(*path);
        }
    }
    return uri;
}

void Compiler::publish_diagnostics(const std::string& uri,
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

void Compiler::clear_diagnostics(const std::string& uri) {
    protocol::PublishDiagnosticsParams params;
    params.uri = uri;
    params.diagnostics = {};
    peer.send_notification(params);
}

et::task<bool> Compiler::ensure_pch(Session& session,
                                    const std::string& directory,
                                    const std::vector<std::string>& arguments) {
    auto path_id = session.path_id;
    auto path = workspace.path_pool.resolve(path_id);
    auto& text = session.text;
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        // No preamble directives — PCH would be empty. Clear any stale entry.
        workspace.pch_cache.erase(path_id);
        session.pch_ref.reset();
        co_return true;
    }

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(text).substr(0, bound));

    // Deterministic content-addressed PCH path.
    auto pch_path = path::join(workspace.config.cache_dir,
                               "cache",
                               "pch",
                               std::format("{:016x}.pch", preamble_hash));

    // Reuse existing PCH if preamble content and deps haven't changed.
    if(auto it = workspace.pch_cache.find(path_id); it != workspace.pch_cache.end()) {
        auto& st = it->second;
        if(st.hash == preamble_hash && !st.path.empty() &&
           !deps_changed(workspace.path_pool, st.deps)) {
            st.bound = bound;
            session.pch_ref = Session::PCHRef{path_id, preamble_hash, bound};
            co_return true;
        }
    }

    // Preamble incomplete (user still typing) — defer rebuild, reuse old PCH if available.
    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        co_return workspace.pch_cache.count(path_id) && !workspace.pch_cache[path_id].path.empty();
    }

    // If another coroutine is already building PCH for this file, wait for it.
    if(auto it = workspace.pch_cache.find(path_id);
       it != workspace.pch_cache.end() && it->second.building) {
        co_await it->second.building->wait();
        if(auto it2 = workspace.pch_cache.find(path_id); it2 != workspace.pch_cache.end()) {
            session.pch_ref = Session::PCHRef{path_id, it2->second.hash, it2->second.bound};
        }
        co_return workspace.pch_cache.count(path_id) && !workspace.pch_cache[path_id].path.empty();
    }

    // Register in-flight build so concurrent requests wait on us.
    auto completion = std::make_shared<et::event>();
    workspace.pch_cache[path_id].building = completion;

    // Build a new PCH via stateless worker.
    worker::BuildParams bp;
    bp.kind = worker::BuildKind::BuildPCH;
    bp.file = std::string(path);
    bp.directory = directory;
    bp.arguments = arguments;
    bp.text = text;
    bp.preamble_bound = bound;
    bp.output_path = pch_path;

    LOG_DEBUG("Building PCH for {}, bound={}, output={}", path, bound, pch_path);

    auto result = co_await pool.send_stateless(bp);

    if(!result.has_value() || !result.value().success) {
        LOG_WARN("PCH build failed for {}: {}",
                 path,
                 result.has_value() ? result.value().error : result.error().message);
        workspace.pch_cache[path_id].building.reset();
        completion->set();
        co_return false;
    }

    auto& st = workspace.pch_cache[path_id];
    st.path = result.value().output_path;
    st.bound = bound;
    st.hash = preamble_hash;
    st.deps = capture_deps_snapshot(workspace.path_pool, result.value().deps);
    st.building.reset();

    session.pch_ref = Session::PCHRef{path_id, preamble_hash, bound};

    LOG_INFO("PCH built for {}: {}", path, result.value().output_path);

    // Persist cache metadata after successful build.
    save_cache();

    completion->set();
    co_return true;
}

/// Compile module dependencies, build/reuse PCH, and fill PCM paths.
/// Shared preparation step used by both ensure_compiled() (stateful path)
/// and forward_stateless() (completion/signatureHelp path).
et::task<bool> Compiler::ensure_deps(Session& session,
                                     const std::string& directory,
                                     const std::vector<std::string>& arguments,
                                     std::pair<std::string, uint32_t>& pch,
                                     std::unordered_map<std::string, std::string>& pcms) {
    auto path_id = session.path_id;

    // Compile C++20 module dependencies (PCMs).
    if(workspace.compile_graph && !co_await workspace.compile_graph->compile_deps(path_id)) {
        co_return false;
    }

    // Scan buffer text for module imports that might not be in compile_graph yet.
    // When a user adds `import std;` without saving, the compile_graph (disk-based)
    // doesn't know about the new dependency. Scan the in-memory text to find them.
    {
        auto scan_result = scan(session.text);
        for(auto& mod_name: scan_result.modules) {
            if(mod_name.empty())
                continue;
            bool found = false;
            for(auto& [pid, name]: workspace.path_to_module) {
                if(name == mod_name) {
                    // If PCM not already built, try to build it.
                    if(workspace.pcm_paths.find(pid) == workspace.pcm_paths.end()) {
                        if(workspace.compile_graph && workspace.compile_graph->has_unit(pid)) {
                            co_await workspace.compile_graph->compile_deps(pid);
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
    auto pch_ok = co_await ensure_pch(session, directory, arguments);
    if(pch_ok) {
        if(auto pch_it = workspace.pch_cache.find(path_id); pch_it != workspace.pch_cache.end()) {
            pch = {pch_it->second.path, pch_it->second.bound};
        }
    }

    // Fill all available PCM paths, excluding the file's own PCM
    // to avoid "multiple module declarations".
    fill_pcm_deps(pcms, path_id);

    co_return true;
}

bool Compiler::is_stale(const Session& session) {
    if(session.ast_deps.has_value() && deps_changed(workspace.path_pool, *session.ast_deps))
        return true;

    // Check PCH staleness via the session's pch_ref.
    if(session.pch_ref.has_value()) {
        auto pch_it = workspace.pch_cache.find(session.pch_ref->path_id);
        if(pch_it != workspace.pch_cache.end() &&
           deps_changed(workspace.path_pool, pch_it->second.deps))
            return true;
    }

    return false;
}

void Compiler::record_deps(Session& session, llvm::ArrayRef<std::string> deps) {
    session.ast_deps = capture_deps_snapshot(workspace.path_pool, deps);
}

/// Pull-based compilation entry point for user-opened files.
///
/// Called lazily by forward_query() / forward_build() before every
/// feature request (hover, semantic tokens, etc.). Guarantees that when it
/// returns true the stateful worker assigned to `path_id` holds an up-to-date
/// AST and diagnostics have been published to the client.
///
/// Lifecycle overview (pull-based model):
///
///   didOpen / didChange          – only update Session, mark ast_dirty
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
et::task<bool> Compiler::ensure_compiled(Session& session) {
    auto path_id = session.path_id;

    LOG_DEBUG("ensure_compiled: path_id={} version={} gen={} ast_dirty={}",
              path_id,
              session.version,
              session.generation,
              session.ast_dirty);

    if(!session.ast_dirty) {
        if(!is_stale(session)) {
            co_return true;
        }
        session.ast_dirty = true;
    }

    // If another compile is already in flight, wait for it.
    // This co_await may be cancelled by LSP $/cancelRequest — that's fine,
    // it just means this particular feature request is abandoned.  The
    // detached compile task keeps running independently.
    while(session.compiling) {
        auto pending = session.compiling;
        co_await pending->done.wait();
        if(!session.ast_dirty)
            co_return true;
    }

    // No compile in flight and AST is dirty — launch a detached compile task.
    // The detached task is scheduled via loop.schedule() so it is NOT subject
    // to LSP $/cancelRequest cancellation.  This eliminates the race where
    // cancellation fires the RAII guard, waking all waiters simultaneously
    // and causing them all to start new compiles.
    auto pending_compile = std::make_shared<Session::PendingCompile>();
    session.compiling = pending_compile;

    LOG_INFO("ensure_compiled: launching detached compile path_id={} gen={}",
             path_id,
             session.generation);

    // Capture path_id by value so the detached lambda can re-lookup the session
    // from the sessions map after co_await (DenseMap may invalidate pointers).
    loop.schedule([](Compiler* self,
                     std::uint32_t pid,
                     std::shared_ptr<Session::PendingCompile> pc) -> et::task<> {
        // Re-lookup session from the sessions map (pointer may have been
        // invalidated by DenseMap growth during co_await).
        auto find_session = [&]() -> Session* {
            auto it = self->sessions.find(pid);
            return it != self->sessions.end() ? &it->second : nullptr;
        };

        auto* sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        auto finish_compile = [&]() {
            auto* s = find_session();
            if(s && s->compiling == pc) {
                s->compiling.reset();
            }
            LOG_INFO("ensure_compiled: finish_compile (detached) path_id={}", pid);
            pc->done.set();
        };

        auto gen = sess->generation;
        LOG_INFO("ensure_compiled: starting compile (detached) path_id={} gen={}", pid, gen);

        auto file_path = std::string(self->workspace.path_pool.resolve(pid));
        auto uri = lsp::URI::from_file_path(file_path);
        std::string uri_str = uri.has_value() ? uri->str() : file_path;

        worker::CompileParams params;
        params.path = file_path;
        params.version = sess->version;
        params.text = sess->text;
        if(!self->fill_compile_args(file_path, params.directory, params.arguments, sess)) {
            finish_compile();
            co_return;
        }

        if(!co_await self
                ->ensure_deps(*sess, params.directory, params.arguments, params.pch, params.pcms)) {
            LOG_WARN("Dependency preparation failed for {}, skipping compile", uri_str);
            finish_compile();
            co_return;
        }

        // Re-lookup after co_await (DenseMap may have grown).
        sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        auto result = co_await self->pool.send_stateful(pid, params);

        // Re-lookup after co_await.
        sess = find_session();
        if(!sess) {
            pc->done.set();
            co_return;
        }

        if(sess->generation != gen) {
            LOG_INFO("ensure_compiled: generation mismatch ({} vs {}) for {}",
                     sess->generation,
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

        sess->ast_dirty = false;
        pc->succeeded = true;
        self->record_deps(*sess, result.value().deps);

        // Store open file index from the stateful worker's TUIndex.
        if(!result.value().tu_index_data.empty()) {
            auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
            OpenFileIndex ofi;
            ofi.file_index = std::move(tu_index.main_file_index);
            ofi.symbols = std::move(tu_index.symbols);
            ofi.content = sess->text;
            ofi.mapper.emplace(ofi.content, lsp::PositionEncoding::UTF16);
            sess->file_index = std::move(ofi);
        }

        finish_compile();

        // Publish diagnostics AFTER marking compile as done, so that concurrent
        // forward_query() calls can proceed immediately.
        self->publish_diagnostics(uri_str, sess->version, result.value().diagnostics);
        if(self->on_indexing_needed)
            self->on_indexing_needed();
    }(this, path_id, pending_compile));

    // Wait for the detached compile to finish.  If this wait is cancelled
    // by LSP $/cancelRequest, the detached task continues unaffected.
    co_await pending_compile->done.wait();

    co_return !session.ast_dirty;
}

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind, Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }

    if(session.ast_dirty) {
        co_return serde_raw{"null"};
    }

    worker::QueryParams wp{kind, path};
    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value());
}

Compiler::RawResult Compiler::forward_query(worker::QueryKind kind,
                                            const protocol::Position& position,
                                            Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    if(!co_await ensure_compiled(session)) {
        co_return serde_raw{"null"};
    }

    if(session.ast_dirty) {
        co_return serde_raw{"null"};
    }

    lsp::PositionMapper mapper(session.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};

    worker::QueryParams wp{kind, path, *offset};
    auto result = co_await pool.send_stateful(path_id, wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value());
}

Compiler::RawResult Compiler::forward_build(worker::BuildKind kind,
                                            const protocol::Position& position,
                                            Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    worker::BuildParams wp;
    wp.kind = kind;
    wp.file = path;
    wp.version = session.version;
    wp.text = session.text;
    if(!fill_compile_args(path, wp.directory, wp.arguments, &session)) {
        co_return serde_raw{};
    }

    if(!co_await ensure_deps(session, wp.directory, wp.arguments, wp.pch, wp.pcms)) {
        co_return serde_raw{};
    }

    lsp::PositionMapper mapper(wp.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(!offset)
        co_return serde_raw{"null"};
    wp.offset = *offset;

    auto result = co_await pool.send_stateless(wp);
    if(!result.has_value()) {
        co_return serde_raw{};
    }
    co_return std::move(result.value().result_json);
}

Compiler::RawResult Compiler::handle_completion(const protocol::Position& position,
                                                Session& session) {
    auto path_id = session.path_id;
    auto path = std::string(workspace.path_pool.resolve(path_id));

    lsp::PositionMapper mapper(session.text, lsp::PositionEncoding::UTF16);
    auto offset = mapper.to_offset(position);
    if(offset) {
        auto pctx = detect_completion_context(session.text, *offset);
        if(pctx.kind == CompletionContext::IncludeQuoted ||
           pctx.kind == CompletionContext::IncludeAngled) {
            co_return complete_include(pctx, path);
        }
        if(pctx.kind == CompletionContext::Import) {
            co_return complete_import(pctx);
        }
    }

    co_return co_await forward_build(worker::BuildKind::Completion, position, session);
}

PreambleCompletionContext Compiler::detect_completion_context(const std::string& text,
                                                              uint32_t offset) {
    auto line_start = text.rfind('\n', offset > 0 ? offset - 1 : 0);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;

    auto line_end = text.find('\n', offset);
    if(line_end == std::string::npos)
        line_end = text.size();

    auto line = llvm::StringRef(text).slice(line_start, offset);
    auto trimmed = line.ltrim();

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
        return {};
    }

    auto import_check = trimmed;
    if(import_check.consume_front("export") && !import_check.empty() &&
       !std::isalnum(import_check[0])) {
        import_check = import_check.ltrim();
    }
    if(import_check.consume_front("import") &&
       (import_check.empty() || !std::isalnum(import_check[0]))) {
        import_check = import_check.ltrim();
        auto rest_of_line = llvm::StringRef(text).slice(line_start, line_end);
        if(!rest_of_line.contains(';')) {
            return {CompletionContext::Import, import_check.str()};
        }
    }

    return {};
}

et::serde::RawValue Compiler::complete_include(const PreambleCompletionContext& ctx,
                                               llvm::StringRef path) {
    std::string directory;
    std::vector<std::string> arguments;
    if(!fill_compile_args(path, directory, arguments))
        return serde_raw{"[]"};

    std::vector<const char*> args_ptrs;
    args_ptrs.reserve(arguments.size());
    for(auto& arg: arguments) {
        args_ptrs.push_back(arg.c_str());
    }

    auto search_config = extract_search_config(args_ptrs, directory);
    DirListingCache dir_cache;
    auto resolved = resolve_search_config(search_config, dir_cache);

    unsigned start_idx = 0;
    if(ctx.kind == CompletionContext::IncludeAngled) {
        start_idx = resolved.angled_start_idx;
    }

    llvm::StringRef prefix_ref(ctx.prefix);
    llvm::StringRef dir_prefix;
    llvm::StringRef file_prefix = prefix_ref;
    auto slash_pos = prefix_ref.rfind('/');
    if(slash_pos != llvm::StringRef::npos) {
        dir_prefix = prefix_ref.slice(0, slash_pos);
        file_prefix = prefix_ref.slice(slash_pos + 1, llvm::StringRef::npos);
    }

    std::vector<protocol::CompletionItem> items;
    llvm::StringSet<> seen;

    for(unsigned i = start_idx; i < resolved.dirs.size(); ++i) {
        auto& search_dir = resolved.dirs[i];

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
    return serde_raw{json ? std::move(*json) : "[]"};
}

et::serde::RawValue Compiler::complete_import(const PreambleCompletionContext& ctx) {
    std::vector<protocol::CompletionItem> items;
    llvm::StringRef prefix_ref(ctx.prefix);

    for(auto& [path_id, module_name]: workspace.path_to_module) {
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
    return serde_raw{json ? std::move(*json) : "[]"};
}

}  // namespace clice
