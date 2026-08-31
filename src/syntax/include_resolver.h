#pragma once

#include <cstdint>
#include <optional>

#include "command/search_config.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

namespace clice {

struct FileTable;

struct ResolveResult {
    /// The resolved absolute path (stack-allocated for paths < 256 chars).
    llvm::SmallString<256> path;

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

/// Per-operation view over directory listings. Instead of calling stat()
/// for each candidate path, directory contents are listed once via
/// readdir() and checked with in-memory set lookups thereafter — this is
/// dramatically faster on Windows where individual stat() calls are very
/// expensive (~10x slower than Linux).
///
/// With `shared` set, listings live in the FileTable's directory
/// compartment and survive the operation: the first use inside an
/// operation validates a cached listing by the directory's own mtime (one
/// stat instead of one readdir), later uses within the operation trust
/// the validation (`validated`). Without it, `dirs` owns the listings and
/// they die with the view — the standalone mode of tools and tests.
///
/// TODO: on case-insensitive filesystems (macOS HFS+/APFS, Windows NTFS),
/// the readdir-based first-component optimization in resolve_include may
/// produce false negatives when the #include casing differs from disk.
struct DirListingCache {
    FileTable* shared = nullptr;
    llvm::StringSet<> validated;
    llvm::StringMap<llvm::StringSet<>> dirs;
};

/// A search directory with a pre-resolved pointer to its cached entries.
/// The pointer is stable because StringMap allocates entries on the heap.
struct ResolvedSearchDir {
    llvm::StringRef path;
    const llvm::StringSet<>* entries;  // Never null after resolve_search_config().
};

/// Pre-resolved version of SearchConfig — all directory lookups are resolved
/// to direct pointers, eliminating StringMap lookups during include resolution.
struct ResolvedSearchConfig {
    llvm::SmallVector<ResolvedSearchDir> dirs;
    unsigned angled_start_idx = 0;
    unsigned system_start_idx = 0;
    unsigned after_start_idx = 0;
};

/// Resolve a single directory to its cached StringSet.
/// Returns a stable pointer into the DirListingCache.
/// On cache miss, lazily populates via readdir().
const llvm::StringSet<>* resolve_dir(llvm::StringRef dir,
                                     DirListingCache& cache,
                                     StatCounters* counters = nullptr);

/// Pre-resolve a SearchConfig against a populated DirListingCache.
/// Call once per config after dir cache pre-population, then reuse
/// the result for all resolve_include() calls with that config.
ResolvedSearchConfig resolve_search_config(const SearchConfig& config, DirListingCache& cache);

/// Resolve an include directive using pre-resolved config and includer entries.
///
/// @param filename         Raw include name (without delimiters)
/// @param is_angled        Whether this is a <...> include
/// @param includer_entries Pre-resolved StringSet for the includer's directory (may be null)
/// @param includer_dir     Directory of the file containing the #include
/// @param is_include_next  Whether this is #include_next
/// @param found_dir_idx    For #include_next: the search dir index of the includer
/// @param config           Pre-resolved search configuration
/// @return Resolved path and the search dir index, or nullopt if not found
std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             const llvm::StringSet<>* includer_entries,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             unsigned found_dir_idx,
                                             const ResolvedSearchConfig& config,
                                             DirListingCache& dir_cache,
                                             StatCounters* stat_counters = nullptr);

/// Convenience overload: resolves config and includer_dir on the fly.
/// Use for tests and one-off calls where pre-resolution overhead doesn't matter.
std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             unsigned found_dir_idx,
                                             const SearchConfig& config,
                                             DirListingCache& dir_cache,
                                             StatCounters* stat_counters = nullptr);

}  // namespace clice
