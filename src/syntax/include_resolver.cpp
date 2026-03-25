#include "syntax/include_resolver.h"

#include <chrono>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

namespace {

/// Look up the directory listing in cache, populating on first access.
/// Returns the StringSet of filenames in the directory.
llvm::StringSet<>& get_dir_entries(llvm::StringRef dir,
                                   DirListingCache& cache,
                                   StatCounters* counters) {
    auto dir_it = cache.dirs.find(dir);
    if(dir_it == cache.dirs.end()) {
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
            counters->us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        }

        dir_it = cache.dirs.try_emplace(dir, std::move(entries)).first;
    } else {
        if(counters) {
            counters->dir_hits++;
        }
    }

    return dir_it->second;
}

/// Check if a file exists in a directory using cached listings.
/// Avoids constructing the full path — dir and filename are supplied separately.
bool check_file_in_dir(llvm::StringRef dir,
                       llvm::StringRef filename,
                       DirListingCache& cache,
                       StatCounters* counters) {
    if(counters) {
        counters->lookups++;
    }
    return get_dir_entries(dir, cache, counters).contains(filename);
}

/// Check if a full path exists using cached directory listings.
bool check_file(llvm::StringRef path, DirListingCache& cache, StatCounters* counters) {
    auto dir = llvm::sys::path::parent_path(path);
    auto name = llvm::sys::path::filename(path);
    return check_file_in_dir(dir, name, cache, counters);
}

}  // namespace

std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             unsigned found_dir_idx,
                                             const SearchConfig& config,
                                             DirListingCache& dir_cache,
                                             StatCounters* stat_counters) {
    // 1. Absolute path: return directly if exists.
    if(llvm::sys::path::is_absolute(filename)) {
        if(check_file(filename, dir_cache, stat_counters)) {
            return ResolveResult{llvm::SmallString<256>(filename), 0};
        }
        return std::nullopt;
    }

    // Reusable candidate buffer to avoid repeated SmallString construction.
    llvm::SmallString<256> candidate;

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next) {
        unsigned start = found_dir_idx + 1;
        for(unsigned i = start; i < config.dirs.size(); ++i) {
            if(check_file_in_dir(config.dirs[i].path, filename, dir_cache, stat_counters)) {
                candidate = config.dirs[i].path;
                llvm::sys::path::append(candidate, filename);
                return ResolveResult{candidate, i};
            }
        }
        return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && !includer_dir.empty()) {
        if(check_file_in_dir(includer_dir, filename, dir_cache, stat_counters)) {
            candidate = includer_dir;
            llvm::sys::path::append(candidate, filename);
            return ResolveResult{candidate, 0};
        }
    }

    // 4. Search directories from appropriate start index.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        if(check_file_in_dir(config.dirs[i].path, filename, dir_cache, stat_counters)) {
            candidate = config.dirs[i].path;
            llvm::sys::path::append(candidate, filename);
            return ResolveResult{candidate, i};
        }
    }

    return std::nullopt;
}

}  // namespace clice
