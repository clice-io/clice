/// Completing the header name of an `#include` or a `__has_include`.

module clice:feature.code_completion.include_paths;

import :command.search_config;
import :feature.code_completion.cursor;
import :support.prelude;
import :syntax.position;
import :vfs.dir_cache;

namespace clice::feature {

/// Entry in the include path completion result.
struct IncludeCandidate {
    std::string name;
    bool is_directory = false;

    /// Its rank, ascending. A hidden name (`.git/`) follows all the others,
    /// and a file no header would be named as (`README.md`, `main.cpp`), or
    /// a builtin header that must not be included directly, the rest. Then
    /// the better the typed name matches: the whole name, a prefix, words of
    /// it (`v` finds `string_view`). Then where the search found it: the
    /// includer's directory, the quoted then the angled search directories,
    /// the system ones the command names, last the toolchain's own; in
    /// search order within each, after the others of its group when the
    /// name is reserved (`_`-led: a library's internals, libc++'s
    /// `__algorithm/`, MinGW's `_mingw.h`).
    std::uint64_t order = 0;
};

/// Return header and directory names matching the typed name in the given search paths.
/// @param config        The command's search directories.
/// @param includer_dir  Directory of the file being edited, searched first by "" includes.
/// @param prefix        Partially-typed include path (e.g. "vec" or "sys/").
/// @param angled        True for <> includes, false for "" includes.
/// @param scope         The request's directory listings.
std::vector<IncludeCandidate> complete_include_path(const SearchConfig& config,
                                                    llvm::StringRef includer_dir,
                                                    llvm::StringRef prefix,
                                                    bool angled,
                                                    vfs::Scope& scope);

/// The paths an `#include` being typed (`completing`) may go on with in
/// `text`, the file at `path`, as `search` finds them: a directory leaves
/// the path open for the `/` typed next, a header closes the directive.
auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList;

}  // namespace clice::feature
