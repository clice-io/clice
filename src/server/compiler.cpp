#include "server/compiler.h"

#include <chrono>
#include <format>
#include <ranges>
#include <string>

#include "command/search_config.h"
#include "eventide/serde/json/json.h"
#include "server/master_server.h"  // For DocumentState
#include "server/protocol.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/scan.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice {

// =========================================================================
// Static helpers
// =========================================================================

static std::uint64_t hash_file(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if(!buf)
        return 0;
    return llvm::xxh3_64bits((*buf)->getBuffer());
}

static DepsSnapshot capture_deps_snapshot(PathPool& pool, llvm::ArrayRef<std::string> deps) {
    DepsSnapshot snap;
    snap.build_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    snap.path_ids.reserve(deps.size());
    snap.hashes.reserve(deps.size());
    for(const auto& file: deps) {
        snap.path_ids.push_back(pool.intern(file));
        snap.hashes.push_back(hash_file(file));
    }
    return snap;
}

static bool deps_changed(const PathPool& pool, const DepsSnapshot& snap) {
    for(std::size_t i = 0; i < snap.path_ids.size(); ++i) {
        auto path = pool.resolve(snap.path_ids[i]);
        llvm::sys::fs::file_status status;
        if(auto ec = llvm::sys::fs::status(path, status)) {
            if(snap.hashes[i] != 0)
                return true;
            continue;
        }

        auto current_mtime = llvm::sys::toTimeT(status.getLastModificationTime());
        if(current_mtime <= snap.build_at)
            continue;

        auto current_hash = hash_file(path);
        if(current_hash != snap.hashes[i])
            return true;
    }
    return false;
}

// =========================================================================
// Cache serialization structs (anonymous namespace)
// =========================================================================

namespace {

struct CacheDepEntry {
    std::uint32_t path;
    std::uint64_t hash;
};

struct CachePCHEntry {
    std::string filename;
    std::uint32_t source_file;
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

// =========================================================================
// Constructor / Destructor
// =========================================================================

Compiler::Compiler(PathPool& path_pool,
                   WorkerPool& pool,
                   Indexer& indexer,
                   const CliceConfig& config,
                   CompilationDatabase& cdb,
                   DependencyGraph& dep_graph) :
    path_pool(path_pool),
    pool(pool),
    indexer(indexer),
    config(config),
    cdb(cdb),
    dep_graph(dep_graph) {}

Compiler::~Compiler() {
    cancel_all();
}

void Compiler::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms) const {
    for(auto& [pid, pcm_path]: pcm_paths) {
        auto mod_it = path_to_module.find(pid);
        if(mod_it != path_to_module.end()) {
            pcms[mod_it->second] = pcm_path;
        }
    }
}

void Compiler::cancel_all() {
    if(compile_graph) {
        compile_graph->cancel_all();
    }
}

// =========================================================================
// Cache persistence
// =========================================================================

void Compiler::load_cache() {
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

void Compiler::save_cache() {
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

void Compiler::cleanup_cache(int max_age_days) {
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

// =========================================================================
// Module graph
// =========================================================================

void Compiler::build_module_map() {
    for(auto& [module_name, path_ids]: dep_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }
}

void Compiler::init_compile_graph() {
    if(path_to_module.empty()) {
        LOG_INFO("No C++20 modules detected, skipping CompileGraph");
        return;
    }

    // Lazy dependency resolver.
    auto resolve = [this](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = path_pool.resolve(path_id);
        auto results = cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty())
            return {};

        auto& ctx = results[0];
        auto scan_result = scan_precise(ctx.arguments, ctx.directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = dep_graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        if(!scan_result.module_name.empty() && !scan_result.is_interface_unit) {
            auto mod_ids = dep_graph.lookup_module(scan_result.module_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }

        return deps;
    };

    // Dispatch: builds PCM via stateless worker.
    auto dispatch = [this](std::uint32_t path_id) -> et::task<bool> {
        auto mod_it = path_to_module.find(path_id);
        if(mod_it == path_to_module.end())
            co_return false;

        auto file_path = std::string(path_pool.resolve(path_id));

        worker::BuildPCMParams pcm_params;
        pcm_params.file = file_path;
        if(!fill_compile_args(file_path, pcm_params.directory, pcm_params.arguments))
            co_return false;

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

        if(!result.value().tu_index_data.empty()) {
            indexer.merge(result.value().tu_index_data.data(),
                          result.value().tu_index_data.size());
        }

        save_cache();
        co_return true;
    };

    compile_graph = std::make_unique<CompileGraph>(std::move(dispatch), std::move(resolve));
    LOG_INFO("CompileGraph initialized with {} module(s)", path_to_module.size());
}

// =========================================================================
// Compile argument resolution
// =========================================================================

bool Compiler::fill_compile_args(llvm::StringRef path,
                                 std::string& directory,
                                 std::vector<std::string>& arguments) {
    auto path_id = path_pool.intern(path);

    auto active_it = active_contexts.find(path_id);
    if(active_it != active_contexts.end()) {
        return fill_header_context_args(path, path_id, directory, arguments);
    }

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

    return fill_header_context_args(path, path_id, directory, arguments);
}

bool Compiler::fill_header_context_args(llvm::StringRef path,
                                        std::uint32_t path_id,
                                        std::string& directory,
                                        std::vector<std::string>& arguments) {
    const HeaderFileContext* ctx_ptr = nullptr;
    auto ctx_it = header_file_contexts.find(path_id);
    auto active_it = active_contexts.find(path_id);
    if(ctx_it != header_file_contexts.end()) {
        if(active_it != active_contexts.end() && ctx_it->second.host_path_id != active_it->second) {
            header_file_contexts.erase(ctx_it);
        } else {
            ctx_ptr = &ctx_it->second;
        }
    }
    if(!ctx_ptr) {
        // resolve_header_context needs documents — use empty map as fallback
        // since fill_header_context_args is called from fill_compile_args
        // which doesn't have access to documents. The resolve will use disk content.
        llvm::DenseMap<std::uint32_t, DocumentState> empty_docs;
        auto resolved = resolve_header_context(path_id, empty_docs);
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
        LOG_WARN("fill_header_context_args: host {} has no CDB entry", host_path);
        return false;
    }

    auto& host_ctx = host_results.front();
    directory = host_ctx.directory.str();
    arguments.clear();

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

// =========================================================================
// Header context
// =========================================================================

std::optional<HeaderFileContext>
    Compiler::resolve_header_context(std::uint32_t header_path_id,
                                     const llvm::DenseMap<std::uint32_t, DocumentState>& documents) {
    auto hosts = dep_graph.find_host_sources(header_path_id);
    if(hosts.empty()) {
        LOG_DEBUG("resolve_header_context: no host sources for path_id={}", header_path_id);
        return std::nullopt;
    }

    std::uint32_t host_path_id = 0;
    std::vector<std::uint32_t> chain;
    auto active_it = active_contexts.find(header_path_id);
    if(active_it != active_contexts.end()) {
        auto preferred = active_it->second;
        auto preferred_path = path_pool.resolve(preferred);
        auto results = cdb.lookup(preferred_path, {.suppress_logging = true});
        if(!results.empty()) {
            auto c = dep_graph.find_include_chain(preferred, header_path_id);
            if(!c.empty()) {
                host_path_id = preferred;
                chain = std::move(c);
            }
        }
    }

    if(chain.empty()) {
        for(auto candidate: hosts) {
            auto candidate_path = path_pool.resolve(candidate);
            auto results = cdb.lookup(candidate_path, {.suppress_logging = true});
            if(results.empty())
                continue;
            auto c = dep_graph.find_include_chain(candidate, header_path_id);
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

    std::string preamble;
    for(std::size_t i = 0; i + 1 < chain.size(); ++i) {
        auto cur_id = chain[i];
        auto next_id = chain[i + 1];

        auto cur_path = path_pool.resolve(cur_id);
        auto next_path = path_pool.resolve(next_id);
        auto next_filename = llvm::sys::path::filename(next_path);

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

        llvm::StringRef content_ref(content);
        std::size_t line_start = 0;
        std::size_t include_line_start = std::string::npos;
        while(line_start <= content_ref.size()) {
            auto newline_pos = content_ref.find('\n', line_start);
            auto line_end =
                (newline_pos == llvm::StringRef::npos) ? content_ref.size() : newline_pos;
            auto line = content_ref.slice(line_start, line_end).trim();

            if(line.starts_with("#include") || line.starts_with("# include")) {
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

        preamble += std::format("#line 1 \"{}\"\n", cur_path.str());
        if(include_line_start != std::string::npos) {
            preamble += content_ref.substr(0, include_line_start).str();
        } else {
            LOG_DEBUG("resolve_header_context: include line for {} not found in {}, emitting full",
                      next_filename,
                      cur_path);
            preamble += content;
        }
    }

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

void Compiler::switch_context(std::uint32_t path_id, std::uint32_t context_path_id) {
    active_contexts[path_id] = context_path_id;
    header_file_contexts.erase(path_id);
    pch_states.erase(path_id);
    ast_deps.erase(path_id);
}

std::optional<std::uint32_t> Compiler::get_active_context(std::uint32_t path_id) const {
    auto it = active_contexts.find(path_id);
    if(it != active_contexts.end())
        return it->second;
    return std::nullopt;
}

void Compiler::invalidate_host_contexts(std::uint32_t host_path_id,
                                        llvm::SmallVectorImpl<std::uint32_t>& stale_headers) {
    for(auto& [hdr_id, hdr_ctx]: header_file_contexts) {
        if(hdr_ctx.host_path_id == host_path_id)
            stale_headers.push_back(hdr_id);
    }
    for(auto hdr_id: stale_headers) {
        header_file_contexts.erase(hdr_id);
    }
}

// =========================================================================
// Dependency preparation
// =========================================================================

et::task<bool> Compiler::ensure_pch(std::uint32_t path_id,
                                    llvm::StringRef path,
                                    const std::string& text,
                                    const std::string& directory,
                                    const std::vector<std::string>& arguments) {
    auto bound = compute_preamble_bound(text);
    if(bound == 0) {
        pch_states.erase(path_id);
        co_return true;
    }

    auto preamble_hash = llvm::xxh3_64bits(llvm::StringRef(text).substr(0, bound));

    auto pch_path =
        path::join(config.cache_dir, "cache", "pch", std::format("{:016x}.pch", preamble_hash));

    if(auto it = pch_states.find(path_id); it != pch_states.end()) {
        auto& st = it->second;
        if(st.hash == preamble_hash && !st.path.empty() && !deps_changed(path_pool, st.deps)) {
            st.bound = bound;
            co_return true;
        }
    }

    if(!is_preamble_complete(text, bound)) {
        LOG_DEBUG("Preamble incomplete for {}, deferring PCH rebuild", path);
        co_return pch_states.count(path_id) && !pch_states[path_id].path.empty();
    }

    if(auto it = pch_states.find(path_id); it != pch_states.end() && it->second.building) {
        co_await it->second.building->wait();
        co_return !pch_states[path_id].path.empty();
    }

    auto completion = std::make_shared<et::event>();
    pch_states[path_id].building = completion;

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

    auto& st = pch_states[path_id];
    st.path = result.value().pch_path;
    st.bound = bound;
    st.hash = preamble_hash;
    st.deps = capture_deps_snapshot(path_pool, result.value().deps);
    st.building.reset();

    LOG_INFO("PCH built for {}: {}", path, result.value().pch_path);

    if(!result.value().tu_index_data.empty()) {
        indexer.merge(result.value().tu_index_data.data(),
                      result.value().tu_index_data.size());
    }

    save_cache();

    completion->set();
    co_return true;
}

et::task<bool> Compiler::ensure_deps(std::uint32_t path_id,
                                     llvm::StringRef path,
                                     const std::string& text,
                                     const std::string& directory,
                                     const std::vector<std::string>& arguments,
                                     std::pair<std::string, uint32_t>& pch,
                                     std::unordered_map<std::string, std::string>& pcms) {
    if(compile_graph && !co_await compile_graph->compile_deps(path_id)) {
        co_return false;
    }

    {
        auto scan_result = scan(text);
        for(auto& mod_name: scan_result.modules) {
            if(mod_name.empty())
                continue;
            bool found = false;
            for(auto& [pid, name]: path_to_module) {
                if(name == mod_name) {
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

    auto pch_ok = co_await ensure_pch(path_id, path, text, directory, arguments);
    if(pch_ok) {
        if(auto pch_it = pch_states.find(path_id); pch_it != pch_states.end()) {
            pch = {pch_it->second.path, pch_it->second.bound};
        }
    }

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

// =========================================================================
// Staleness detection
// =========================================================================

bool Compiler::is_stale(std::uint32_t path_id) {
    auto ast_deps_it = ast_deps.find(path_id);
    if(ast_deps_it != ast_deps.end() && deps_changed(path_pool, ast_deps_it->second))
        return true;

    auto pch_it = pch_states.find(path_id);
    if(pch_it != pch_states.end() && deps_changed(path_pool, pch_it->second.deps))
        return true;

    return false;
}

void Compiler::record_deps(std::uint32_t path_id, llvm::ArrayRef<std::string> deps) {
    ast_deps[path_id] = capture_deps_snapshot(path_pool, deps);
}

// =========================================================================
// Lifecycle events
// =========================================================================

void Compiler::on_file_closed(std::uint32_t path_id) {
    if(compile_graph && compile_graph->has_unit(path_id)) {
        compile_graph->update(path_id);
    }
    pch_states.erase(path_id);
    ast_deps.erase(path_id);
}

llvm::SmallVector<std::uint32_t> Compiler::on_file_saved(std::uint32_t path_id) {
    llvm::SmallVector<std::uint32_t> dirtied;
    if(compile_graph) {
        auto result = compile_graph->update(path_id);
        for(auto id: result) {
            dirtied.push_back(id);
            pcm_paths.erase(id);
            pcm_states.erase(id);
        }
    }
    return dirtied;
}

}  // namespace clice
