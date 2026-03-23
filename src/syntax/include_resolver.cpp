#include "syntax/include_resolver.h"

#include <chrono>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace {

/// Check if a file exists, with cache. Uses synchronous access() — much faster
/// than async stat for dependency scanning since we only need existence checks.
std::optional<llvm::StringRef> stat_file(llvm::StringRef path,
                                         llvm::StringMap<std::optional<std::string>>& cache,
                                         StatCounters* counters) {
    auto it = cache.find(path);
    if(it != cache.end()) {
        if(counters) {
            counters->hits++;
        }
        if(it->second.has_value()) {
            return llvm::StringRef(it->second.value());
        }
        return std::nullopt;
    }

    if(counters) {
        counters->calls++;
    }
    auto t0 = std::chrono::steady_clock::now();
    bool exists = llvm::sys::fs::exists(path);
    auto t1 = std::chrono::steady_clock::now();
    if(counters) {
        counters->us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    }

    if(exists) {
        auto [entry, _] = cache.try_emplace(path, path.str());
        return llvm::StringRef(entry->second.value());
    }

    cache.try_emplace(path, std::nullopt);
    return std::nullopt;
}

}  // namespace

et::task<std::optional<ResolveResult>, et::error>
    resolve_include(llvm::StringRef filename,
                    bool is_angled,
                    llvm::StringRef includer_dir,
                    bool is_include_next,
                    unsigned found_dir_idx,
                    const SearchConfig& config,
                    llvm::StringMap<std::optional<std::string>>& stat_cache,
                    [[maybe_unused]] et::event_loop& loop,
                    StatCounters* stat_counters) {
    // 1. Absolute path: return directly if exists.
    if(llvm::sys::path::is_absolute(filename)) {
        if(auto result = stat_file(filename, stat_cache, stat_counters)) {
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
            if(auto result = stat_file(candidate, stat_cache, stat_counters)) {
                co_return ResolveResult{result->str(), i};
            }
        }
        co_return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && !includer_dir.empty()) {
        llvm::SmallString<256> candidate(includer_dir);
        llvm::sys::path::append(candidate, filename);
        if(auto result = stat_file(candidate, stat_cache, stat_counters)) {
            co_return ResolveResult{result->str(), 0};
        }
    }

    // 4. Search directories from appropriate start index.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        llvm::SmallString<256> candidate(config.dirs[i].path);
        llvm::sys::path::append(candidate, filename);
        if(auto result = stat_file(candidate, stat_cache, stat_counters)) {
            co_return ResolveResult{result->str(), i};
        }
    }

    co_return std::nullopt;
}

}  // namespace clice
