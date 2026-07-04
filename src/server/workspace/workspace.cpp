#include "server/workspace/workspace.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <tuple>

#include "command/search_config.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/preamble_synthesis.h"
#include "syntax/scan.h"

#include "kota/codec/json/json.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/xxhash.h"

namespace clice {

bool Workspace::is_synthesized_artifact(llvm::StringRef path) const {
    if(config.project.cache_dir.empty()) {
        return false;
    }
    auto artifact_dir = path::join(config.project.cache_dir, "header_context");
    return path.starts_with(artifact_dir);
}

HeaderMode Workspace::header_mode(llvm::StringRef path, std::uint32_t path_id) const {
    // Keep in sync with the client's C++ fragment detection
    // (editors/vscode/src/feature/context.ts).
    if(path.ends_with(".def") || path.ends_with(".inc") || path.ends_with(".inl") ||
       path.ends_with(".tpp") || path.ends_with(".ipp")) {
        return HeaderMode::NeedsContext;
    }
    if(auto it = header_modes.find(path_id); it != header_modes.end()) {
        return it->second;
    }
    return HeaderMode::Unknown;
}

std::uint32_t Workspace::count_occurrences(std::uint32_t host_id, std::uint32_t target_id) const {
    auto chain = dep_graph.find_include_chain(host_id, target_id);
    if(chain.size() < 2) {
        return 0;
    }
    auto includer_path = path_pool.resolve(chain[chain.size() - 2]);
    auto target_path = path_pool.resolve(target_id);
    auto buf = llvm::MemoryBuffer::getFile(includer_path);
    if(!buf) {
        return 0;
    }
    auto null_resolver =
        [](llvm::StringRef, bool, bool, llvm::StringRef) -> std::optional<std::string> {
        return std::nullopt;
    };
    return count_include_occurrences((*buf)->getBuffer(),
                                     includer_path,
                                     target_path,
                                     null_resolver);
}

void Workspace::forget_self_contained(std::uint32_t path_id) {
    if(auto it = header_modes.find(path_id);
       it != header_modes.end() && it->second == HeaderMode::SelfContained) {
        header_modes.erase(it);
    }
}

llvm::SmallVector<std::uint32_t> Workspace::rank_hosts(std::uint32_t header_path_id,
                                                       llvm::ArrayRef<std::uint32_t> hosts) const {
    auto header_path = path_pool.resolve(header_path_id);
    auto header_stem = llvm::sys::path::stem(header_path);
    auto header_dir = llvm::sys::path::parent_path(header_path);

    auto score = [&](std::uint32_t host_id) -> std::tuple<int, int, std::size_t> {
        auto host_path = path_pool.resolve(host_id);
        int stem_match = llvm::sys::path::stem(host_path) == header_stem ? 0 : 1;
        int same_dir = llvm::sys::path::parent_path(host_path) == header_dir ? 0 : 1;
        // Longer shared prefix means "closer" in the tree; negate for
        // ascending sort.
        std::size_t common = 0;
        auto n = std::min(host_path.size(), header_path.size());
        while(common < n && host_path[common] == header_path[common]) {
            ++common;
        }
        return {stem_match, same_dir, n - common};
    };

    llvm::SmallVector<std::uint32_t> ranked(hosts.begin(), hosts.end());
    std::ranges::sort(ranked, [&](std::uint32_t a, std::uint32_t b) {
        auto sa = score(a), sb = score(b);
        if(sa != sb) {
            return sa < sb;
        }
        return path_pool.resolve(a) < path_pool.resolve(b);
    });
    return ranked;
}

void Workspace::rescan_includes(std::uint32_t path_id) {
    auto path = path_pool.resolve(path_id);
    dep_graph.clear_includes(path_id);

    if(auto buf = llvm::MemoryBuffer::getFile(path)) {
        // Search paths come from the file's own command, or a host's for
        // headers without a CDB entry; the synthesized default still
        // resolves quote includes via the includer directory.
        llvm::StringRef cmd_path = path;
        if(!cdb.has_entry(path)) {
            for(auto host: rank_hosts(path_id, dep_graph.find_host_sources(path_id))) {
                auto host_path = path_pool.resolve(host);
                if(cdb.has_entry(host_path)) {
                    cmd_path = host_path;
                    break;
                }
            }
        }

        std::vector<std::string> rule_append, rule_remove;
        config.match_rules(cmd_path, rule_append, rule_remove);
        auto cmds = cdb.lookup(cmd_path, {.remove = rule_remove, .append = rule_append});

        // Resolve under every configuration: an include may only be
        // reachable through the -I set of a non-first CDB entry. The local
        // index serves as config id — prior keys were just cleared and
        // consumers read the union.
        auto includes = scan((*buf)->getBuffer()).includes;
        DirListingCache dir_cache;
        auto dir = llvm::sys::path::parent_path(path);
        for(std::uint32_t ci = 0; ci < cmds.size(); ++ci) {
            auto& cmd = cmds[ci];
            toolchain.resolve_or_warn(cmd);
            auto argv = cmd.to_argv();
            auto search_config = extract_search_config(argv, cmd.resolved.directory);
            auto resolved_config = resolve_search_config(search_config, dir_cache);
            auto entries = resolve_dir(dir, dir_cache);

            llvm::SmallVector<std::uint32_t> ids;
            for(auto& include: includes) {
                auto resolved = resolve_include(include.path,
                                                include.is_angled,
                                                entries,
                                                dir,
                                                include.is_include_next,
                                                0,
                                                resolved_config,
                                                dir_cache);
                if(resolved) {
                    ids.push_back(path_pool.intern(resolved->path));
                }
            }
            dep_graph.set_includes(path_id, ci, std::move(ids));
        }
    }

    dep_graph.build_reverse_map();
}

llvm::SmallVector<std::uint32_t> Workspace::on_file_saved(std::uint32_t path_id) {
    // Contexts must see includes added/removed by this save.
    rescan_includes(path_id);

    context_epoch += 1;

    llvm::SmallVector<std::uint32_t> dirtied;

    // Re-scan the saved file for module declarations and update path_to_module.
    auto file_path = path_pool.resolve(path_id);
    if(auto buf = llvm::MemoryBuffer::getFile(file_path)) {
        auto result = scan((*buf)->getBuffer());
        if(!result.module_name.empty()) {
            path_to_module[path_id] = std::move(result.module_name);
        } else {
            path_to_module.erase(path_id);
        }
    }

    if(compile_graph) {
        auto result = compile_graph->update(path_id);
        for(auto id: result) {
            dirtied.push_back(id);
            pcm_paths.erase(id);
            pcm_cache.erase(id);
        }
    }
    return dirtied;
}

void Workspace::on_file_closed(std::uint32_t path_id) {
    if(compile_graph && compile_graph->has_unit(path_id)) {
        compile_graph->update(path_id);
    }
    // PCH entries are content-keyed and may be shared with other sessions;
    // blob eviction is the CacheStore's job, so nothing to clean up here.
}

std::uint64_t hash_file(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if(!buf)
        return 0;
    return llvm::xxh3_64bits((*buf)->getBuffer());
}

DepsSnapshot capture_deps_snapshot(PathPool& pool, llvm::ArrayRef<std::string> deps) {
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

bool deps_changed(const PathPool& pool, const DepsSnapshot& snap) {
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

namespace {

struct CacheDepEntry {
    std::uint32_t path;  // index into CacheData::paths
    std::uint64_t hash;
};

struct CachePCHEntry {
    std::string key;  // CacheStore key in the "pch" namespace
    std::uint32_t bound;
    std::int64_t build_at;
    std::vector<CacheDepEntry> deps;

    // Preamble share of the inactive-region scan; consumed on PCH reuse.
    std::vector<std::uint32_t> inactive_regions;
    std::vector<std::uint8_t> open_conditionals;
};

struct CachePCMEntry {
    std::string key;  // CacheStore key in the "pcm" namespace
    std::uint32_t source_file;
    std::string module_name;
    std::int64_t build_at;
    std::vector<CacheDepEntry> deps;
};

struct CacheModeEntry {
    std::uint32_t file;          // index into CacheData::paths
    std::uint32_t mode;          // HeaderMode
    std::uint64_t content_hash;  // header contents the verdict was scored on
};

struct CacheContextEntry {
    std::uint32_t file;  // index into CacheData::paths
    std::uint32_t host;  // index into CacheData::paths; ~0u = none
    std::uint32_t occurrence;
    std::string command_hash;
};

struct CacheArtifactEntry {
    std::uint32_t file;  // index into CacheData::paths
    std::uint32_t host;  // index into CacheData::paths
};

struct CacheData {
    std::vector<std::string> paths;
    std::vector<CachePCHEntry> pch;
    std::vector<CachePCMEntry> pcm;
    std::vector<CacheModeEntry> header_modes;
    std::vector<CacheContextEntry> contexts;
    std::vector<CacheArtifactEntry> artifacts;
};

}  // namespace

void Workspace::load_cache() {
    if(!store)
        return;

    auto cache_path = path::join(store->base_dir(), "cache.json");
    auto content = fs::read(cache_path);
    if(!content) {
        LOG_DEBUG("No cache.json found at {}", cache_path);
        return;
    }

    CacheData data;
    auto status = kota::codec::json::from_json(*content, data);
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
            deps.path_ids.push_back(path_pool.intern(dep_path));
            deps.hashes.push_back(dep.hash);
        }
        return deps;
    };

    for(auto& entry: data.pch) {
        auto pch_path = store->lookup("pch", entry.key);
        if(!pch_path)
            continue;

        auto& st = pch_cache[entry.key];
        st.path = *pch_path;
        st.bound = entry.bound;
        st.deps = load_deps(entry.build_at, entry.deps);
        st.inactive_regions = entry.inactive_regions;
        st.open_conditionals = entry.open_conditionals;

        LOG_DEBUG("Loaded cached PCH: {} -> {}", entry.key, *pch_path);
    }

    for(auto& entry: data.pcm) {
        auto pcm_path = store->lookup("pcm", entry.key);
        auto source = resolve(entry.source_file);
        if(!pcm_path || source.empty())
            continue;

        auto path_id = path_pool.intern(source);
        pcm_cache[path_id] = {*pcm_path, entry.key, load_deps(entry.build_at, entry.deps)};
        pcm_paths[path_id] = *pcm_path;

        LOG_DEBUG("Loaded cached PCM: {} (module {}) -> {}", source, entry.module_name, *pcm_path);
    }

    for(auto& entry: data.header_modes) {
        auto file = resolve(entry.file);
        if(file.empty() || static_cast<HeaderMode>(entry.mode) != HeaderMode::NeedsContext)
            continue;
        // The verdict is tied to the header's contents — a file edited
        // while the server was down must re-earn its trial.
        if(entry.content_hash != 0 && hash_file(file) != entry.content_hash)
            continue;
        auto id = path_pool.intern(file);
        header_modes[id] = HeaderMode::NeedsContext;
        header_mode_hashes[id] = entry.content_hash;
    }

    for(auto& entry: data.contexts) {
        auto file = resolve(entry.file);
        if(file.empty())
            continue;
        SavedContext saved;
        if(entry.host != ~0u) {
            auto host = resolve(entry.host);
            if(host.empty())
                continue;
            saved.host_path_id = path_pool.intern(host);
        }
        if(entry.occurrence != ~0u) {
            saved.occurrence = entry.occurrence;
        }
        saved.command_hash = entry.command_hash;
        saved_contexts[path_pool.intern(file)] = std::move(saved);
    }

    for(auto& entry: data.artifacts) {
        auto file = resolve(entry.file);
        auto host = resolve(entry.host);
        if(file.empty() || host.empty())
            continue;
        synthesized_hosts[file] = path_pool.intern(host);
    }

    LOG_INFO("Loaded cache.json: {} PCH entries, {} PCM entries, {} context choices",
             pch_cache.size(),
             pcm_cache.size(),
             saved_contexts.size());
}

void Workspace::save_cache() {
    if(!store)
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

    for(auto& e: pch_cache) {
        auto& st = e.second;
        if(st.path.empty())
            continue;

        CachePCHEntry entry;
        entry.key = e.getKey().str();
        entry.bound = st.bound;
        entry.build_at = st.deps.build_at;
        entry.inactive_regions = st.inactive_regions;
        entry.open_conditionals = st.open_conditionals;
        for(std::size_t i = 0; i < st.deps.path_ids.size(); ++i) {
            entry.deps.push_back({intern(st.deps.path_ids[i]), st.deps.hashes[i]});
        }
        data.pch.push_back(std::move(entry));
    }

    for(auto& [path_id, st]: pcm_cache) {
        if(st.path.empty())
            continue;

        CachePCMEntry entry;
        entry.key = st.key;
        entry.source_file = intern(path_id);
        auto mod_it = path_to_module.find(path_id);
        entry.module_name = mod_it != path_to_module.end() ? mod_it->second : "";
        entry.build_at = st.deps.build_at;
        for(std::size_t i = 0; i < st.deps.path_ids.size(); ++i) {
            entry.deps.push_back({intern(st.deps.path_ids[i]), st.deps.hashes[i]});
        }
        data.pcm.push_back(std::move(entry));
    }

    for(auto& [path_id, mode]: header_modes) {
        if(mode != HeaderMode::NeedsContext)
            continue;
        auto hash_it = header_mode_hashes.find(path_id);
        data.header_modes.push_back({intern(path_id),
                                     static_cast<std::uint32_t>(mode),
                                     hash_it != header_mode_hashes.end() ? hash_it->second : 0});
    }

    for(auto& entry: synthesized_hosts) {
        auto [it, inserted] = index_map.try_emplace(entry.getKey().str(),
                                                    static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(entry.getKey().str());
        }
        data.artifacts.push_back({it->second, intern(entry.second)});
    }

    for(auto& [path_id, saved]: saved_contexts) {
        CacheContextEntry entry;
        entry.file = intern(path_id);
        entry.host = saved.host_path_id != no_path_id ? intern(saved.host_path_id) : ~0u;
        entry.occurrence = saved.occurrence.value_or(~0u);
        entry.command_hash = saved.command_hash;
        data.contexts.push_back(std::move(entry));
    }

    auto json_str = kota::codec::json::to_json(data);
    if(!json_str) {
        LOG_WARN("Failed to serialize cache.json");
        return;
    }

    auto cache_path = path::join(store->base_dir(), "cache.json");
    // Stage inside this instance's store tmp directory: other instances of
    // the same workspace must not clobber each other's half-written file.
    auto pid = llvm::sys::Process::getProcessId();
    auto tmp_path = path::join(store->base_dir(), "tmp", std::to_string(pid), "cache.json");
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

void Workspace::build_module_map() {
    for(auto& [module_name, path_ids]: dep_graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = module_name.str();
        }
    }
}

void Workspace::fill_pcm_deps(std::unordered_map<std::string, std::string>& pcms,
                              std::uint32_t exclude_path_id) const {
    for(auto& [pid, pcm_path]: pcm_paths) {
        if(pid == exclude_path_id)
            continue;
        auto mod_it = path_to_module.find(pid);
        if(mod_it != path_to_module.end()) {
            pcms[mod_it->second] = pcm_path;
        }
    }
}

void Workspace::cancel_all() {
    if(compile_graph) {
        compile_graph->cancel_all();
    }
}

}  // namespace clice
