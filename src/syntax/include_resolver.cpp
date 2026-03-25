#include "syntax/include_resolver.h"

#include <chrono>

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace clice {

const llvm::StringSet<>* resolve_dir(llvm::StringRef dir, DirListingCache& cache,
                                     StatCounters* counters) {
    auto it = cache.dirs.find(dir);
    if(it != cache.dirs.end()) {
        if(counters) {
            counters->dir_hits++;
        }
        return &it->second;
    }

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

    auto [new_it, _] = cache.dirs.try_emplace(dir, std::move(entries));
    return &new_it->second;
}

ResolvedSearchConfig resolve_search_config(const SearchConfig& config, DirListingCache& cache) {
    ResolvedSearchConfig resolved;
    resolved.angled_start_idx = config.angled_start_idx;
    resolved.dirs.reserve(config.dirs.size());
    for(auto& dir: config.dirs) {
        resolved.dirs.push_back({dir.path, resolve_dir(dir.path, cache)});
    }
    return resolved;
}

namespace {

/// Check if a file exists in a directory, handling multi-component include paths.
/// For simple filenames (no '/'), checks pre-resolved entries directly.
/// For multi-component paths like "llvm/Support/raw_ostream.h", constructs the
/// full path and resolves the actual parent subdirectory via DirListingCache.
bool check_in_dir(llvm::StringRef dir_path,
                  const llvm::StringSet<>* entries,
                  llvm::StringRef filename,
                  bool is_simple,
                  DirListingCache& dir_cache,
                  StatCounters* counters) {
    if(counters) counters->lookups++;

    if(is_simple) {
        return entries->contains(filename);
    }

    // Quick rejection: check if first path component exists in pre-resolved
    // entries. For "llvm/Support/raw_ostream.h", check if "llvm" exists in
    // the search dir listing. Most search dirs won't have it, so we skip
    // the expensive full path construction + subdirectory resolution.
    // Skip this for relative paths starting with "." or ".." (e.g. "../foo.h").
    auto first_sep = filename.find_first_of("/\\");
    auto first_component = filename.substr(0, first_sep);
    if(first_component != "." && first_component != "..") {
        if(!entries->contains(first_component)) {
            return false;
        }
    }

    // First component matched — construct full path, resolve actual subdirectory.
    llvm::SmallString<256> full;
    full = dir_path;
    llvm::sys::path::append(full, filename);
    auto parent = llvm::sys::path::parent_path(full);
    auto name = llvm::sys::path::filename(full);
    auto* sub_entries = resolve_dir(parent, dir_cache, counters);
    return sub_entries->contains(name);
}

}  // namespace

std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             const llvm::StringSet<>* includer_entries,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             unsigned found_dir_idx,
                                             const ResolvedSearchConfig& config,
                                             DirListingCache& dir_cache,
                                             StatCounters* stat_counters) {
    // 1. Absolute path: check directly via stat().
    if(llvm::sys::path::is_absolute(filename)) {
        if(llvm::sys::fs::exists(filename)) {
            return ResolveResult{llvm::SmallString<256>(filename), 0};
        }
        return std::nullopt;
    }

    // Check if filename has path separators (multi-component like "llvm/Support/foo.h").
    bool is_simple = filename.find('/') == llvm::StringRef::npos &&
                     filename.find('\\') == llvm::StringRef::npos;

    llvm::SmallString<256> candidate;

    // 2. For #include_next, start from found_dir_idx + 1.
    if(is_include_next) {
        unsigned start = found_dir_idx + 1;
        for(unsigned i = start; i < config.dirs.size(); ++i) {
            if(check_in_dir(config.dirs[i].path, config.dirs[i].entries, filename, is_simple,
                            dir_cache, stat_counters)) {
                candidate = config.dirs[i].path;
                llvm::sys::path::append(candidate, filename);
                return ResolveResult{candidate, i};
            }
        }
        return std::nullopt;
    }

    // 3. Quoted include: try includer's directory first.
    if(!is_angled && includer_entries) {
        if(check_in_dir(includer_dir, includer_entries, filename, is_simple, dir_cache,
                         stat_counters)) {
            candidate = includer_dir;
            llvm::sys::path::append(candidate, filename);
            return ResolveResult{candidate, 0};
        }
    }

    // 4. Search directories from appropriate start index.
    unsigned start = is_angled ? config.angled_start_idx : 0;
    for(unsigned i = start; i < config.dirs.size(); ++i) {
        if(check_in_dir(config.dirs[i].path, config.dirs[i].entries, filename, is_simple,
                        dir_cache, stat_counters)) {
            candidate = config.dirs[i].path;
            llvm::sys::path::append(candidate, filename);
            return ResolveResult{candidate, i};
        }
    }

    return std::nullopt;
}

std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             unsigned found_dir_idx,
                                             const SearchConfig& config,
                                             DirListingCache& dir_cache,
                                             StatCounters* stat_counters) {
    auto resolved_config = resolve_search_config(config, dir_cache);
    const llvm::StringSet<>* includer_entries =
        includer_dir.empty() ? nullptr : resolve_dir(includer_dir, dir_cache, stat_counters);
    return resolve_include(filename, is_angled, includer_entries, includer_dir,
                           is_include_next, found_dir_idx, resolved_config, dir_cache,
                           stat_counters);
}

}  // namespace clice
