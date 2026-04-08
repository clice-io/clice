#include "server/artifact_cache.h"

#include <algorithm>
#include <chrono>
#include <format>

#include "eventide/serde/json/json.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/xxhash.h"

namespace clice {

// ── Free utilities (moved from workspace.cpp) ──────────────────────────────

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

// ── ArtifactCache ──────────────────────────────────────────────────────────

ArtifactEntry* ArtifactCache::lookup(ArtifactKey key) {
    auto it = entries.find(key);
    if(it == entries.end())
        return nullptr;
    it->second.last_access = std::chrono::steady_clock::now();
    return &it->second;
}

void ArtifactCache::insert(ArtifactKey key, ArtifactEntry entry) {
    entry.last_access = std::chrono::steady_clock::now();
    entries[key] = std::move(entry);
}

void ArtifactCache::erase(ArtifactKey key) {
    entries.erase(key);
}

void ArtifactCache::collect_dependents(ArtifactKey root,
                                       llvm::SmallVectorImpl<ArtifactKey>& result) const {
    // Find all entries that list `root` in their inputs (direct dependents).
    for(auto& [key, entry]: entries) {
        if(llvm::is_contained(entry.inputs, root)) {
            result.push_back(key);
            // Recurse: find entries depending on this dependent.
            collect_dependents(key, result);
        }
    }
}

llvm::SmallVector<ArtifactKey> ArtifactCache::invalidate(ArtifactKey key) {
    llvm::SmallVector<ArtifactKey> invalidated;

    // Collect all transitive dependents first.
    collect_dependents(key, invalidated);
    invalidated.push_back(key);

    // Remove all invalidated entries.
    for(auto k: invalidated) {
        entries.erase(k);
    }

    return invalidated;
}

void ArtifactCache::evict(std::size_t max_entries) {
    if(entries.size() <= max_entries)
        return;

    // Collect all keys referenced as inputs (these cannot be evicted).
    llvm::DenseMap<ArtifactKey, bool> referenced;
    for(auto& [key, entry]: entries) {
        for(auto input: entry.inputs) {
            referenced[input] = true;
        }
    }

    // Collect eviction candidates (not referenced by others).
    struct Candidate {
        ArtifactKey key;
        std::chrono::steady_clock::time_point last_access;
    };

    std::vector<Candidate> candidates;
    for(auto& [key, entry]: entries) {
        if(!referenced.count(key)) {
            candidates.push_back({key, entry.last_access});
        }
    }

    // Sort by last_access ascending (oldest first).
    std::ranges::sort(candidates, {}, &Candidate::last_access);

    // Evict oldest candidates until we're under the limit.
    auto to_evict = entries.size() - max_entries;
    for(std::size_t i = 0; i < to_evict && i < candidates.size(); ++i) {
        // Invalidate cascades to dependents (which shouldn't exist since
        // we only evict unreferenced entries, but be safe).
        invalidate(candidates[i].key);
    }
}

bool ArtifactCache::deps_changed(const PathPool& pool, ArtifactKey key) const {
    auto it = entries.find(key);
    if(it == entries.end())
        return true;  // Entry gone — treat as changed.
    return clice::deps_changed(pool, it->second.deps);
}

ArtifactKey ArtifactCache::compute_key(llvm::StringRef content,
                                       llvm::ArrayRef<std::string> arguments,
                                       llvm::ArrayRef<ArtifactKey> input_keys) {
    // Hash: content + all arguments + input artifact keys.
    // Arguments are separated by null bytes to avoid collisions.
    std::string hash_input;
    hash_input.reserve(content.size() + arguments.size() * 32 + input_keys.size() * 8);
    hash_input.append(content.data(), content.size());
    for(auto& arg: arguments) {
        hash_input.push_back('\0');
        hash_input.append(arg);
    }
    for(auto key: input_keys) {
        hash_input.append(reinterpret_cast<const char*>(&key), sizeof(key));
    }
    return llvm::xxh3_64bits(llvm::StringRef(hash_input));
}

ArtifactKey ArtifactCache::compute_key_with_path(llvm::StringRef path,
                                                 llvm::ArrayRef<std::string> arguments,
                                                 llvm::ArrayRef<ArtifactKey> input_keys) {
    // For PCM: include file path in the hash (identity matters).
    std::string hash_input;
    hash_input.reserve(path.size() + arguments.size() * 32 + input_keys.size() * 8);
    hash_input.append(path.data(), path.size());
    for(auto& arg: arguments) {
        hash_input.push_back('\0');
        hash_input.append(arg);
    }
    for(auto key: input_keys) {
        hash_input.append(reinterpret_cast<const char*>(&key), sizeof(key));
    }
    return llvm::xxh3_64bits(llvm::StringRef(hash_input));
}

ArtifactKey ArtifactCache::compute_key(llvm::StringRef content,
                                       llvm::ArrayRef<const char*> flags,
                                       llvm::ArrayRef<ArtifactKey> input_keys) {
    std::string hash_input;
    hash_input.reserve(content.size() + flags.size() * 32 + input_keys.size() * 8);
    hash_input.append(content.data(), content.size());
    for(auto* flag: flags) {
        hash_input.push_back('\0');
        hash_input.append(flag);
    }
    for(auto key: input_keys) {
        hash_input.append(reinterpret_cast<const char*>(&key), sizeof(key));
    }
    return llvm::xxh3_64bits(llvm::StringRef(hash_input));
}

ArtifactKey ArtifactCache::compute_key_with_path(llvm::StringRef path,
                                                 llvm::ArrayRef<const char*> flags,
                                                 llvm::ArrayRef<ArtifactKey> input_keys) {
    std::string hash_input;
    hash_input.reserve(path.size() + flags.size() * 32 + input_keys.size() * 8);
    hash_input.append(path.data(), path.size());
    for(auto* flag: flags) {
        hash_input.push_back('\0');
        hash_input.append(flag);
    }
    for(auto key: input_keys) {
        hash_input.append(reinterpret_cast<const char*>(&key), sizeof(key));
    }
    return llvm::xxh3_64bits(llvm::StringRef(hash_input));
}

// ── Persistence ────────────────────────────────────────────────────────────

namespace {

struct CacheDepEntry {
    std::uint32_t path;  // index into CacheData::paths
    std::uint64_t hash;
};

struct CacheArtifactEntry {
    std::string filename;
    std::uint64_t key;
    std::int64_t build_at;
    std::vector<std::uint64_t> inputs;
    std::vector<CacheDepEntry> deps;
};

struct CacheData {
    std::vector<std::string> paths;
    std::vector<CacheArtifactEntry> artifacts;
};

}  // namespace

void ArtifactCache::save(const PathPool& pool, llvm::StringRef cache_dir) const {
    if(cache_dir.empty())
        return;

    CacheData data;
    std::unordered_map<std::string, std::uint32_t> index_map;

    auto intern = [&](std::uint32_t runtime_path_id) -> std::uint32_t {
        auto path = std::string(pool.resolve(runtime_path_id));
        auto [it, inserted] =
            index_map.try_emplace(path, static_cast<std::uint32_t>(data.paths.size()));
        if(inserted) {
            data.paths.push_back(path);
        }
        return it->second;
    };

    for(auto& [key, entry]: entries) {
        if(entry.path.empty())
            continue;

        CacheArtifactEntry ae;
        ae.filename = std::string(path::filename(entry.path));
        ae.key = key;
        ae.build_at = entry.deps.build_at;
        ae.inputs.assign(entry.inputs.begin(), entry.inputs.end());
        for(std::size_t i = 0; i < entry.deps.path_ids.size(); ++i) {
            ae.deps.push_back({intern(entry.deps.path_ids[i]), entry.deps.hashes[i]});
        }
        data.artifacts.push_back(std::move(ae));
    }

    auto json_str = eventide::serde::json::to_json(data);
    if(!json_str) {
        LOG_WARN("Failed to serialize artifact cache");
        return;
    }

    auto cache_path = path::join(cache_dir, "cache", "cache.json");
    auto tmp_path = cache_path + ".tmp";
    auto write_result = fs::write(tmp_path, *json_str);
    if(!write_result) {
        LOG_WARN("Failed to write cache.json.tmp: {}", write_result.error().message());
        return;
    }
    auto rename_result = fs::rename(tmp_path, cache_path);
    if(!rename_result) {
        LOG_WARN("Failed to rename cache.json.tmp: {}", rename_result.error().message());
    }
}

void ArtifactCache::load(PathPool& pool, llvm::StringRef cache_dir) {
    if(cache_dir.empty())
        return;

    auto cache_path = path::join(cache_dir, "cache", "cache.json");
    auto content = fs::read(cache_path);
    if(!content) {
        LOG_DEBUG("No cache.json found at {}", cache_path);
        return;
    }

    CacheData data;
    auto status = eventide::serde::json::from_json(*content, data);
    if(!status) {
        LOG_WARN("Failed to parse cache.json");
        return;
    }

    auto resolve = [&](std::uint32_t idx) -> llvm::StringRef {
        return idx < data.paths.size() ? llvm::StringRef(data.paths[idx]) : "";
    };

    std::size_t loaded = 0;
    for(auto& ae: data.artifacts) {
        // Determine artifact subdirectory from filename extension.
        llvm::StringRef fname(ae.filename);
        llvm::StringRef subdir = fname.ends_with(".pch") ? "cache/pch" : "cache/pcm";
        auto artifact_path = path::join(cache_dir, subdir, ae.filename);

        if(!llvm::sys::fs::exists(artifact_path))
            continue;

        ArtifactEntry entry;
        entry.path = artifact_path;
        entry.inputs.assign(ae.inputs.begin(), ae.inputs.end());
        entry.deps.build_at = ae.build_at;
        for(auto& dep: ae.deps) {
            auto dep_path = resolve(dep.path);
            if(dep_path.empty())
                continue;
            entry.deps.path_ids.push_back(pool.intern(dep_path));
            entry.deps.hashes.push_back(dep.hash);
        }
        entry.last_access = std::chrono::steady_clock::now();

        entries[ae.key] = std::move(entry);
        ++loaded;
    }

    LOG_INFO("Loaded artifact cache: {} entries", loaded);
}

void ArtifactCache::cleanup(llvm::StringRef cache_dir, int max_age_days) {
    if(cache_dir.empty())
        return;

    auto now = std::chrono::system_clock::now();
    auto max_age = std::chrono::hours(max_age_days * 24);

    for(auto* subdir: {"cache/pch", "cache/pcm"}) {
        auto dir = path::join(cache_dir, subdir);
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

}  // namespace clice
