module;

#include "modules/prelude.h"

module clice:syntax.include_resolver;

import :command.search_config;
import :vfs.dir_cache;

namespace clice {

struct ResolveResult {
    /// The resolved absolute path (stack-allocated for paths < 256 chars).
    llvm::SmallString<256> path;

    /// The index in SearchConfig::dirs where this file was found; none
    /// when it was found next to its includer or by absolute path. Used
    /// for #include_next to resume searching from found_dir_idx + 1.
    std::optional<unsigned> found_dir_idx;
};

/// A search directory with its listing for the operation, held by the
/// operation's vfs::Scope.
struct ResolvedSearchDir {
    llvm::StringRef path;
    const vfs::Listing* listing;  // Never null after resolve_search_config().
};

/// Pre-resolved version of SearchConfig — all directory lookups are resolved
/// to direct pointers, eliminating StringMap lookups during include resolution.
struct ResolvedSearchConfig {
    llvm::SmallVector<ResolvedSearchDir> dirs;
    unsigned angled_start_idx = 0;
    unsigned system_start_idx = 0;
    unsigned after_start_idx = 0;
};

/// Pre-resolve a SearchConfig against the operation's listings. Call once
/// per config, then reuse the result for all resolve_include() calls with
/// that config.
ResolvedSearchConfig resolve_search_config(const SearchConfig& config, vfs::Scope& scope);

/// Resolve an include directive using pre-resolved config and includer listing.
///
/// @param filename         Raw include name (without delimiters)
/// @param is_angled        Whether this is a <...> include
/// @param includer_listing Listing of the includer's directory (may be null)
/// @param includer_dir     Directory of the file containing the #include
/// @param is_include_next  Whether this is #include_next
/// @param found_dir_idx    For #include_next: the search dir index of the includer;
///                         none (the main file, or one found outside the search
///                         dirs) looks the include up like a plain one, as clang does
/// @param config           Pre-resolved search configuration
/// @return Resolved path and the search dir index, or nullopt if not found
std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             const vfs::Listing* includer_listing,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             std::optional<unsigned> found_dir_idx,
                                             const ResolvedSearchConfig& config,
                                             vfs::Scope& scope);

/// Convenience overload: resolves config and includer_dir on the fly.
/// Use for tests and one-off calls where pre-resolution overhead doesn't matter.
std::optional<ResolveResult> resolve_include(llvm::StringRef filename,
                                             bool is_angled,
                                             llvm::StringRef includer_dir,
                                             bool is_include_next,
                                             std::optional<unsigned> found_dir_idx,
                                             const SearchConfig& config,
                                             vfs::Scope& scope);

}  // namespace clice
