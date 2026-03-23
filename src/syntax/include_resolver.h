#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/command.h"
#include "eventide/async/async.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace et = eventide;

struct ResolveResult {
    /// The resolved absolute path.
    std::string path;

    /// The index in SearchConfig::dirs where this file was found.
    /// Used for #include_next to resume searching from found_dir_idx + 1.
    unsigned found_dir_idx = 0;
};

/// Counters for stat() call tracking during include resolution.
struct StatCounters {
    std::size_t calls = 0;   // Actual filesystem stat() calls (cache misses).
    std::size_t hits = 0;    // Cache hits (no syscall).
    std::int64_t us = 0;     // Microseconds spent in actual stat() calls.
};

/// Resolve an include directive to an absolute file path (async version).
/// Uses eventide's async fs::stat() for non-blocking filesystem access.
///
/// @param filename       Raw include name (without delimiters)
/// @param is_angled      Whether this is a <...> include
/// @param includer_dir   Directory of the file containing the #include
/// @param is_include_next Whether this is #include_next (start from found_dir_idx + 1)
/// @param found_dir_idx  For #include_next: the search dir index of the includer
/// @param config         The search configuration to use
/// @param stat_cache     Cache for filesystem stat() results
/// @param loop           Event loop for async I/O
/// @return Resolved path and the search dir index, or nullopt if not found
et::task<std::optional<ResolveResult>, et::error>
    resolve_include(llvm::StringRef filename,
                    bool is_angled,
                    llvm::StringRef includer_dir,
                    bool is_include_next,
                    unsigned found_dir_idx,
                    const SearchConfig& config,
                    llvm::StringMap<std::optional<std::string>>& stat_cache,
                    et::event_loop& loop,
                    StatCounters* stat_counters = nullptr);

}  // namespace clice
