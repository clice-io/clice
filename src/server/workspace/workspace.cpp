#include "server/workspace/workspace.h"

#include <algorithm>
#include <chrono>

#include "support/filesystem.h"
#include "support/logging.h"
#include "syntax/scan.h"

#include "kota/codec/json/json.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

namespace lsp = kota::ipc::lsp;

/// Find the tightest (innermost) occurrence containing `offset` via binary search.
const static index::Occurrence* lookup_occurrence(const std::vector<index::Occurrence>& occs,
                                                  std::uint32_t offset) {
    auto it = std::ranges::lower_bound(occs, offset, {}, [](const index::Occurrence& o) {
        return o.range.end;
    });
    const index::Occurrence* best = nullptr;
    while(it != occs.end() && it->range.contains(offset)) {
        if(!best || (it->range.end - it->range.begin) < (best->range.end - best->range.begin)) {
            best = &*it;
        }
        ++it;
    }
    return best;
}

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    OpenFileIndex::find_occurrence(std::uint32_t offset) const {
    if(!mapper)
        return std::nullopt;
    auto* occ = lookup_occurrence(file_index.occurrences, offset);
    if(!occ)
        return std::nullopt;
    auto start = mapper->to_position(occ->range.begin);
    auto end = mapper->to_position(occ->range.end);
    if(!start || !end)
        return std::nullopt;
    return std::pair{
        occ->target,
        protocol::Range{*start, *end}
    };
}

std::optional<std::pair<index::SymbolHash, protocol::Range>>
    MergedIndexShard::find_occurrence(std::uint32_t offset) const {
    auto* m = mapper();
    if(!m)
        return std::nullopt;
    std::optional<std::pair<index::SymbolHash, protocol::Range>> result;
    index.lookup(offset, [&](const index::Occurrence& o) {
        auto start = m->to_position(o.range.begin);
        auto end = m->to_position(o.range.end);
        if(start && end) {
            result = {
                o.target,
                protocol::Range{*start, *end}
            };
        }
        return false;
    });
    return result;
}

llvm::SmallVector<std::uint32_t> Workspace::on_file_saved(std::uint32_t path_id) {
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
    pch_cache.erase(path_id);
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

void Workspace::load_cache() {
    if(config.project.cache_dir.empty())
        return;

    auto cache_path = path::join(config.project.cache_dir, "cache", "cache.json");
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
        auto pch_path = path::join(config.project.cache_dir, "cache", "pch", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pch_path) || source.empty())
            continue;

        auto path_id = path_pool.intern(source);
        auto& st = pch_cache[path_id];
        st.path = pch_path;
        st.hash = entry.hash;
        st.bound = entry.bound;
        st.deps = load_deps(entry.build_at, entry.deps);

        LOG_DEBUG("Loaded cached PCH: {} -> {}", source, pch_path);
    }

    for(auto& entry: data.pcm) {
        auto pcm_path = path::join(config.project.cache_dir, "cache", "pcm", entry.filename);
        auto source = resolve(entry.source_file);
        if(!llvm::sys::fs::exists(pcm_path) || source.empty())
            continue;

        auto path_id = path_pool.intern(source);
        pcm_cache[path_id] = {pcm_path, load_deps(entry.build_at, entry.deps)};
        pcm_paths[path_id] = pcm_path;

        LOG_DEBUG("Loaded cached PCM: {} (module {}) -> {}", source, entry.module_name, pcm_path);
    }

    LOG_INFO("Loaded cache.json: {} PCH entries, {} PCM entries",
             pch_cache.size(),
             pcm_cache.size());
}

void Workspace::save_cache() {
    if(config.project.cache_dir.empty())
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

    for(auto& [path_id, st]: pch_cache) {
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

    for(auto& [path_id, st]: pcm_cache) {
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

    auto json_str = kota::codec::json::to_json(data);
    if(!json_str) {
        LOG_WARN("Failed to serialize cache.json");
        return;
    }

    auto cache_path = path::join(config.project.cache_dir, "cache", "cache.json");
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

void Workspace::cleanup_cache(int max_age_days) {
    if(config.project.cache_dir.empty())
        return;

    auto now = std::chrono::system_clock::now();
    auto max_age = std::chrono::hours(max_age_days * 24);

    for(auto* subdir: {"cache/pch", "cache/pcm"}) {
        auto dir = path::join(config.project.cache_dir, subdir);
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
