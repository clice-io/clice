module;

#include "modules/prelude.h"

module clice:syntax.include_resolver;

import :command.search_config;
import :vfs.dir_cache;
import :vfs.path;

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
///                         none (the source file, or one found outside the search
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

/// Resolve a file a command forces in (`-include`) the way clang does: from
/// the compile's working directory `directory`, then as a quoted include.
std::optional<ResolveResult> resolve_forced_include(llvm::StringRef filename,
                                                    llvm::StringRef directory,
                                                    const ResolvedSearchConfig& config,
                                                    vfs::Scope& scope);

/// The argument, quotes or brackets included, an include directive of
/// `file` names `header` by, the header by identity and the file as the
/// build reaches it: its path below the file's directory (where a quoted
/// include looks first), else the shortest path below one of the command's
/// search directories (resolved, however the command spells them), angled
/// past the quoted segment — whichever of these the command's lookup order
/// actually resolves to `header`, since a shorter one can name a same-named
/// file in an earlier directory. None when that names a library's internal
/// header, which its public headers include and its users never should:
/// libstdc++'s and glibc's `bits/`, libc++'s `__`-prefixed directories and
/// files.
std::optional<std::string> include_argument(CanonicalRef header,
                                            const SearchConfig& search,
                                            const Spelling& file,
                                            vfs::Scope& scope);

}  // namespace clice
