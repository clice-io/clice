#include "syntax/include_resolver.h"

#include <chrono>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace {

/// Check if a file exists using cached directory listings.
/// On first access to a directory, lists all entries via readdir() and caches them.
/// Subsequent lookups in the same directory are pure in-memory set checks.
std::optional<llvm::StringRef> check_file(llvm::StringRef path,
                                          DirListingCache& cache,
                                          StatCounters* counters) {
    if(counters) {
        counters->lookups++;
    }

    auto dir = llvm::sys::path::parent_path(path);
    auto name = llvm::sys::path::filename(path);

    auto dir_it = cache.dirs.find(dir);
    if(dir_it == cache.dirs.end()) {
        // Directory not cached — list it.
        if(counters) {
            counters->dir_listings++;
        }
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

        dir_it = cache.dirs.try_emplace(dir, std::move(entries)).first;
    } else {
        if(counters) {
            counters->dir_hits++;
        }
    }

    if(!dir_it->second.contains(name)) {
        return std::nullopt;
    }

    // File exists — store the full path for a stable StringRef.
    auto [store_it, _] = cache.path_store.try_emplace(path, path.str());
    return llvm::StringRef(store_it->second);
}

}  // namespace

et::task<std::optional<ResolveResult>, et::error>
    resolve_include(llvm::StringRef filename,
                    bool is_angled,
                    llvm::StringRef includer_dir,
                    bool is_include_next,
                    unsigned found_dir_idx,
                    const SearchConfig& config,
                    DirListingCache& dir_cache,
                    [[maybe_unused]] et::event_loop& loop,
                    StatCounters* stat_counters) {
    // 1. Absolute path: return directly if exists.
    if(llvm::sys::path::is_absolute(filename)) {
        if(auto result = check_file(filename, dir_cache, stat_counters)) {
            co_return ResolveResult{result->str(), 0};
        }
        co_return std::nullopt;
    }

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next) {
        unsigned start = found_dir_idx + 1;
        for(unsigned i = start; i < config.dirs.size(); ++i) {
            llvm::SmallString<256> candidate(config.dirs[i].path);
            llvm::sys::path::append(candidate, filename);
            if(auto result = check_file(candidate, dir_cache, stat_counters)) {
                co_return ResolveResult{result->str(), i};
            }
        }
        co_return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && !includer_dir.empty()) {
        llvm::SmallString<256> candidate(includer_dir);
        llvm::sys::path::append(candidate, filename);
        if(auto result = check_file(candidate, dir_cache, stat_counters)) {
            co_return ResolveResult{result->str(), 0};
        }
    }

    // 4. Search directories from appropriate start index.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        llvm::SmallString<256> candidate(config.dirs[i].path);
        llvm::sys::path::append(candidate, filename);
        if(auto result = check_file(candidate, dir_cache, stat_counters)) {
            co_return ResolveResult{result->str(), i};
        }
    }

    co_return std::nullopt;
}

}  // namespace clice
