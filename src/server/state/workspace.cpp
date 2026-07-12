#include "server/state/workspace.h"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <tuple>

#include "command/search_config.h"
#include "server/compiler/context_resolver.h"
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

llvm::SmallVector<std::uint32_t> Workspace::rescan_after_save(std::uint32_t path_id) {
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

std::string discover_compile_commands(const Config& config, llvm::StringRef workspace_root) {
    for(auto& configured: config.project.compile_commands_paths) {
        if(llvm::sys::fs::is_directory(configured)) {
            auto candidate = path::join(configured, "compile_commands.json");
            if(llvm::sys::fs::exists(candidate)) {
                return candidate;
            }
        } else if(llvm::sys::fs::exists(configured)) {
            return configured;
        } else {
            LOG_DEBUG("Configured compile_commands_path not found: {}", configured);
        }
    }

    if(workspace_root.empty()) {
        return {};
    }

    auto try_candidate = [](llvm::StringRef dir) -> std::string {
        auto candidate = path::join(dir, "compile_commands.json");
        if(llvm::sys::fs::exists(candidate)) {
            return candidate;
        }
        return {};
    };

    if(auto found = try_candidate(workspace_root); !found.empty()) {
        return found;
    }

    std::error_code ec;
    for(llvm::sys::fs::directory_iterator it(workspace_root, ec), end; it != end && !ec;
        it.increment(ec)) {
        if(it->type() == llvm::sys::fs::file_type::directory_file) {
            if(auto found = try_candidate(it->path()); !found.empty()) {
                return found;
            }
        }
    }
    return {};
}

std::uint64_t hash_file(llvm::StringRef path) {
    auto buf = llvm::MemoryBuffer::getFile(path);
    if(!buf)
        return 0;
    return llvm::xxh3_64bits((*buf)->getBuffer());
}

DepsSnapshot capture_deps_snapshot(PathPool& pool,
                                   llvm::ArrayRef<DepFile> deps,
                                   std::int64_t build_at) {
    // Files whose mtime falls within the guard of the build start count as
    // "possibly modified during the build" and get no fast-path baseline;
    // one passing hash comparison repairs them (see deps_changed).
    auto baseline_before_ns = fs::stat_baseline_before_ns(build_at);

    DepsSnapshot snap;
    snap.deps.reserve(deps.size());
    for(const auto& file: deps) {
        auto& dep = snap.deps.emplace_back();
        dep.path_id = pool.intern(file.path);
        dep.hash = file.hash;

        llvm::sys::fs::file_status status;
        if(llvm::sys::fs::status(file.path, status)) {
            // The build read it, but it is gone already: record the absence,
            // reappearing counts as a change. Still-missing deliberately
            // counts as unchanged — flagging it would rebuild on every
            // check without ever converging, while the artifact is the
            // last remaining truth for the file (and dependents' recovery
            // is the DiskRemoved cascade's job, not this snapshot's).
            dep.missing = true;
            dep.hash = 0;
            continue;
        }

        auto mtime_ns = fs::mtime_ns(status);
        if(mtime_ns > baseline_before_ns) {
            // Possibly modified during or after the build: this stat may
            // describe content the build never saw, so it must not become a
            // fast-path baseline. The consumed hash stays; the next check
            // compares the disk against it and repairs the fast path on a
            // match.
            continue;
        }

        // Untouched since before the build started — the disk still holds
        // the consumed bytes, so the stat is a trustworthy fast path.
        dep.size = status.getSize();
        dep.mtime_ns = mtime_ns;
        if(dep.hash == 0) {
            // The worker could not hash the consumed bytes; the unchanged
            // mtime proves the disk still holds them, so hash it here.
            dep.hash = hash_file(file.path);
        }
    }
    return snap;
}

bool deps_changed(const PathPool& pool, DepsSnapshot& snap) {
    for(auto& dep: snap.deps) {
        auto path = pool.resolve(dep.path_id);
        llvm::sys::fs::file_status status;
        if(auto ec = llvm::sys::fs::status(path, status)) {
            // Gone now: a change unless it was already missing at build time.
            if(!dep.missing)
                return true;
            continue;
        }
        if(dep.missing) {
            // Missing at build time, present now.
            return true;
        }

        // Layer 1: an unchanged stat proves unchanged content — the baseline
        // is only ever recorded or repaired against the consumed hash.
        auto size = status.getSize();
        auto mtime_ns = fs::mtime_ns(status);
        if(dep.mtime_ns != 0 && dep.size == size && dep.mtime_ns == mtime_ns)
            continue;

        // No trusted hash to compare against: rebuild once to converge.
        if(dep.hash == 0)
            return true;

        // Layer 2: compare the disk against the consumed bytes. hash_file's
        // 0 sentinel (unreadable right now) cannot match and counts as
        // changed — conservative, retried by the next check.
        if(hash_file(path) != dep.hash)
            return true;

        // Touched but not modified — repair the fast path so the next check
        // is a single stat again.
        dep.size = size;
        dep.mtime_ns = mtime_ns;
    }
    return false;
}

namespace {

struct CacheDepEntry {
    std::uint32_t path;  // index into CacheData::paths
    std::uint64_t hash;
    // Defaulted so cache.json files written before the per-dep stat baseline
    // still load: the fields read back zeroed ("no fast path") and the first
    // staleness check re-earns them by hash. Their old entry-level build_at
    // is skipped as an unknown field.
    kota::meta::defaulted<std::uint64_t> size;
    kota::meta::defaulted<std::int64_t> mtime_ns;
    kota::meta::defaulted<bool> missing;
};

struct CachePCHEntry {
    std::string key;  // CacheStore key in the "pch" namespace
    std::uint32_t bound;
    std::vector<CacheDepEntry> deps;
};

struct CachePCMEntry {
    std::string key;  // CacheStore key in the "pcm" namespace
    std::uint32_t source_file;
    std::string module_name;
    std::vector<CacheDepEntry> deps;
};

struct CacheData {
    std::vector<std::string> paths;

    // preamble_format_version the .pch.idx blobs were written with (one
    // binary writes them all). A mismatch drops every PCH entry at load so
    // the pairs rebuild immediately, instead of the mismatch surfacing
    // lazily on the first overlay query — which cannot trigger a rebuild.
    // Old cache.json files read back 0 and are dropped the same way.
    std::uint32_t pch_index_format = 0;

    std::vector<CachePCHEntry> pch;
    std::vector<CachePCMEntry> pcm;
    std::vector<CacheModeEntry> header_modes;
    std::vector<CacheContextEntry> contexts;
    std::vector<CacheArtifactEntry> artifacts;
};

}  // namespace

const std::shared_ptr<index::PreambleState>& PCHState::load_state() {
    if(!state && !index_path.empty()) {
        state = index::PreambleState::load(index_path);
        if(!state) {
            // Unreadable blob: clear the path so queries don't retry the
            // mmap + verification on every call. The pair now looks
            // incomplete and ensure_pch rebuilds it on the next compile.
            LOG_WARN("Failed to open PreambleState blob {}", index_path);
            index_path.clear();
        }
    }
    return state;
}

void Workspace::load_cache(ContextResolver& contexts) {
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

    auto load_deps = [&](const auto& dep_entries) -> DepsSnapshot {
        DepsSnapshot deps;
        for(auto& dep: dep_entries) {
            auto dep_path = resolve(dep.path);
            if(dep_path.empty())
                continue;
            deps.deps.push_back({.path_id = path_pool.intern(dep_path),
                                 .size = dep.size,
                                 .mtime_ns = dep.mtime_ns,
                                 .hash = dep.hash,
                                 .missing = dep.missing});
        }
        return deps;
    };

    bool pch_format_ok = data.pch_index_format == index::preamble_format_version;
    for(auto& entry: data.pch) {
        if(!pch_format_ok) {
            break;
        }

        auto pch_path = store->lookup("pch", entry.key);
        if(!pch_path)
            continue;

        // A PCH without its PreambleState blob is an incomplete pair
        // (crash between the two commits): treat it as absent so the next
        // compile rebuilds both.
        auto index_path = store->lookup_aux("pch", entry.key);
        if(!index_path)
            continue;

        auto& st = pch_cache[entry.key];
        st.path = *pch_path;
        st.bound = entry.bound;
        st.deps = load_deps(entry.deps);
        st.index_path = *index_path;

        LOG_DEBUG("Loaded cached PCH: {} -> {}", entry.key, *pch_path);
    }

    for(auto& entry: data.pcm) {
        auto pcm_path = store->lookup("pcm", entry.key);
        auto source = resolve(entry.source_file);
        if(!pcm_path || source.empty())
            continue;

        // PCM builds now always record at least the module source itself as
        // a dependency, so an empty list marks an entry from before deps
        // were populated — an unvalidatable snapshot that would blindly
        // serve a stale PCM (the key embeds no content). Drop it and let
        // the module rebuild once.
        if(entry.deps.empty()) {
            LOG_INFO("Dropping dep-less cached PCM for {} (pre-upgrade entry)", source);
            continue;
        }

        auto path_id = path_pool.intern(source);
        pcm_cache[path_id] = {*pcm_path, entry.key, load_deps(entry.deps)};
        pcm_paths[path_id] = *pcm_path;

        LOG_DEBUG("Loaded cached PCM: {} (module {}) -> {}", source, entry.module_name, *pcm_path);
    }

    contexts.load_cache_slices(data.header_modes, data.contexts, data.artifacts, resolve);

    LOG_INFO("Loaded cache.json: {} PCH entries, {} PCM entries, {} context choices",
             pch_cache.size(),
             pcm_cache.size(),
             contexts.saved_contexts.size());
}

void Workspace::save_cache(const ContextResolver& contexts) {
    if(!store)
        return;

    CacheData data;
    data.pch_index_format = index::preamble_format_version;
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
        for(auto& dep: st.deps.deps) {
            entry.deps.push_back(
                {intern(dep.path_id), dep.hash, dep.size, dep.mtime_ns, dep.missing});
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
        for(auto& dep: st.deps.deps) {
            entry.deps.push_back(
                {intern(dep.path_id), dep.hash, dep.size, dep.mtime_ns, dep.missing});
        }
        data.pcm.push_back(std::move(entry));
    }

    auto intern_path = [&](llvm::StringRef path) -> std::uint32_t {
        auto [it, inserted] =
            index_map.try_emplace(path.str(), static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path.str());
        }
        return it->second;
    };
    contexts.dump_cache_slices(data.header_modes,
                               data.contexts,
                               data.artifacts,
                               intern,
                               intern_path);

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
