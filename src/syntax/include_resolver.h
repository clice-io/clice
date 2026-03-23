#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/command.h"
#include "eventide/async/async.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace clice {

namespace et = eventide;

struct ResolveResult {
    /// The resolved absolute path.
    std::string path;

    /// The index in SearchConfig::dirs where this file was found.
    /// Used for #include_next to resume searching from found_dir_idx + 1.
    unsigned found_dir_idx = 0;
};

/// Counters for filesystem call tracking during include resolution.
struct StatCounters {
    std::size_t dir_listings = 0;  // Actual readdir() calls (directory cache misses).
    std::size_t dir_hits = 0;      // Directory cache hits (no syscall).
    std::size_t lookups = 0;       // Total file existence lookups.
    std::int64_t us = 0;           // Microseconds spent in filesystem ops.
};

/// Cache of directory listings for fast file existence checks.
/// Instead of calling stat() for each candidate path, we list directory
/// contents once via readdir() and do in-memory set lookups thereafter.
/// This is dramatically faster on Windows where individual stat() calls
/// are very expensive (~10x slower than Linux).
struct DirListingCache {
    /// Maps directory path -> set of entry names in that directory.
    llvm::StringMap<llvm::StringSet<>> dirs;

    /// Maps full path -> stable owning string (for returning StringRef).
    llvm::StringMap<std::string> path_store;
};

/// Resolve an include directive to an absolute file path.
///
/// @param filename       Raw include name (without delimiters)
/// @param is_angled      Whether this is a <...> include
/// @param includer_dir   Directory of the file containing the #include
/// @param is_include_next Whether this is #include_next (start from found_dir_idx + 1)
/// @param found_dir_idx  For #include_next: the search dir index of the includer
/// @param config         The search configuration to use
/// @param dir_cache      Directory listing cache for file existence checks
/// @param loop           Event loop (unused, kept for interface compatibility)
/// @return Resolved path and the search dir index, or nullopt if not found
et::task<std::optional<ResolveResult>, et::error>
    resolve_include(llvm::StringRef filename,
                    bool is_angled,
                    llvm::StringRef includer_dir,
                    bool is_include_next,
                    unsigned found_dir_idx,
                    const SearchConfig& config,
                    DirListingCache& dir_cache,
                    et::event_loop& loop,
                    StatCounters* stat_counters = nullptr);

}  // namespace clice
