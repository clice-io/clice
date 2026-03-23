#include "syntax/include_resolver.h"

#include <chrono>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace {

/// Check if a file exists using cached directory listings.
/// On first access to a directory, lists all entries via readdir() and caches them.
/// Subsequent lookups in the same directory are pure in-memory set checks.
std::optional<std::string> check_file(llvm::StringRef path,
                                      DirListingCache& cache,
                                      StatCounters* counters) {
    if(counters) {
        counters->lookups++;
    }

    auto dir = llvm::sys::path::parent_path(path);
    auto name = llvm::sys::path::filename(path);

    auto& shard = cache.shard_for(dir);
    std::lock_guard lock(shard.mutex);

    auto dir_it = shard.dirs.find(dir);
    if(dir_it == shard.dirs.end()) {
        // Directory not cached — list it.
        // Unlock during expensive readdir, then re-lock to insert.
        if(counters) {
            counters->dir_listings++;
        }
        shard.mutex.unlock();

        auto t0 = std::chrono::steady_clock::now();
        llvm::StringSet<> entries;
        std::error_code ec;
        llvm::sys::fs::directory_iterator di(dir, ec);
        for(; !ec && di != llvm::sys::fs::directory_iterator(); di.increment(ec)) {
            entries.insert(llvm::sys::path::filename(di->path()));
        }
        auto t1 = std::chrono::steady_clock::now();
        if(counters) {
            counters->us +=
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        }

        shard.mutex.lock();
        // Another thread may have inserted while unlocked — try_emplace is safe.
        dir_it = shard.dirs.try_emplace(dir, std::move(entries)).first;
    } else {
        if(counters) {
            counters->dir_hits++;
        }
    }

    if(!dir_it->second.contains(name)) {
        return std::nullopt;
    }

    return path.str();
}

}  // namespace

std::optional<ResolveResult>
    resolve_include(llvm::StringRef filename,
                    bool is_angled,
                    llvm::StringRef includer_dir,
                    bool is_include_next,
                    unsigned found_dir_idx,
                    const SearchConfig& config,
                    DirListingCache& dir_cache,
                    StatCounters* stat_counters) {
    // 1. Absolute path: return directly if exists.
    if(llvm::sys::path::is_absolute(filename)) {
        if(auto result = check_file(filename, dir_cache, stat_counters)) {
            return ResolveResult{std::move(*result), 0};
        }
        return std::nullopt;
    }

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next) {
        unsigned start = found_dir_idx + 1;
        for(unsigned i = start; i < config.dirs.size(); ++i) {
            llvm::SmallString<256> candidate(config.dirs[i].path);
            llvm::sys::path::append(candidate, filename);
            if(auto result = check_file(candidate, dir_cache, stat_counters)) {
                return ResolveResult{std::move(*result), i};
            }
        }
        return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && !includer_dir.empty()) {
        llvm::SmallString<256> candidate(includer_dir);
        llvm::sys::path::append(candidate, filename);
        if(auto result = check_file(candidate, dir_cache, stat_counters)) {
            return ResolveResult{std::move(*result), 0};
        }
    }

    // 4. Search directories from appropriate start index.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        llvm::SmallString<256> candidate(config.dirs[i].path);
        llvm::sys::path::append(candidate, filename);
        if(auto result = check_file(candidate, dir_cache, stat_counters)) {
            return ResolveResult{std::move(*result), i};
        }
    }

    return std::nullopt;
}

}  // namespace clice
