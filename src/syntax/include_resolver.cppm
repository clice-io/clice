module clice:syntax.include_resolver;

import :command.search_config;
import :support.prelude;
import :vfs.dir_cache;
import :vfs.file_system;
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
    bool framework = false;
};

/// The search directories an operation's configurations search, by the
/// names they hold: an include looks in the few directories holding its
/// name rather than in each of a search list hundreds long (Unreal's are).
struct NameIndex {
    /// Index the entries of `listing`, once.
    void add(const vfs::Listing& listing);

    /// The listings that may hold an entry `name`: those holding it, and
    /// those matching names case-insensitively that hold it in another case.
    void holders(llvm::StringRef name, llvm::SmallVectorImpl<const vfs::Listing*>& out) const;

    llvm::DenseSet<const vfs::Listing*> added;

    llvm::StringMap<llvm::SmallVector<const vfs::Listing*, 1>> exact;

    /// Names folded (vfs::fold_case), of the listings matching case-insensitively.
    llvm::StringMap<llvm::SmallVector<const vfs::Listing*, 1>> folded;
};

/// Pre-resolved version of SearchConfig — all directory lookups are resolved
/// to direct pointers, eliminating StringMap lookups during include resolution.
struct ResolvedSearchConfig {
    llvm::SmallVector<ResolvedSearchDir> dirs;
    unsigned angled_start_idx = 0;
    std::shared_ptr<const vfs::HiddenFiles> hidden;

    /// The names `dirs` hold; null for a configuration searched in order.
    const NameIndex* names = nullptr;

    /// Where in `dirs` each of their listings stands, with `names`: twice
    /// for a directory both quoted and angled (`-iquote . -I .`).
    llvm::DenseMap<const vfs::Listing*, llvm::SmallVector<unsigned, 1>> positions;

    /// Whether any of `dirs` is a framework directory, which the index does not serve.
    bool frameworks = false;
};

/// Pre-resolve a SearchConfig against the operation's listings. Call once
/// per config, then reuse the result for all resolve_include() calls with
/// that config. With `names`, which holds the listings of its directories
/// once they are added (NameIndex::add), the index serves its lookups.
ResolvedSearchConfig resolve_search_config(const SearchConfig& config,
                                           vfs::Scope& scope,
                                           const NameIndex* names = nullptr);

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

/// A quoted include found next to its includer, in the directory
/// `includer_dir` listed as `includer_listing`, where clang looks before
/// the search directories; none when it is not there.
std::optional<ResolveResult> resolve_beside(llvm::StringRef filename,
                                            const vfs::Listing& includer_listing,
                                            llvm::StringRef includer_dir,
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

/// How an include directive names a header (IncludeNames::of).
struct IncludeName {
    /// The directories the build finds a header through.
    enum class Found : std::uint8_t {
        /// Next to the including file, or -iquote or -I.
        User,
        /// -isystem, -idirafter or a framework directory.
        System,
        /// The toolchain's own (SearchDir::driver): the standard library's.
        Toolchain,
    };

    /// The path between the delimiters.
    std::string path;

    Found found = Found::User;
};

/// The names an include directive of `file` names headers by, with the
/// command's search directories resolved once for them all. Borrows
/// `search` and `scope`.
class IncludeNames {
public:
    IncludeNames(const SearchConfig& search, const Spelling& file, vfs::Scope& scope);

    /// `header`'s, the header by identity and the file as the build reaches
    /// it: its path below the file's directory (where a quoted include
    /// looks first), else the shortest path below one of the command's
    /// search directories (resolved, however the command spells them),
    /// angled past the quoted segment — whichever of these the command's
    /// lookup order actually resolves to `header`, since a shorter one can
    /// name a same-named file in an earlier directory. None when that names
    /// a library's internal header, which its public headers include and
    /// its users never should: libstdc++'s and glibc's `bits/`, libc++'s
    /// `__`-prefixed directories and files.
    std::optional<IncludeName> of(CanonicalRef header);

private:
    const SearchConfig& search;

    vfs::Scope& scope;

    /// The including file's directory, which the quoted segment searches
    /// first, and its identity.
    Spelling directory;

    CanonicalPath resolved_directory;

    /// The identity of each of `search.dirs`, in its order.
    std::vector<CanonicalPath> resolved_dirs;

    /// The answer for each header asked already.
    llvm::StringMap<std::optional<IncludeName>> known;
};

}  // namespace clice
