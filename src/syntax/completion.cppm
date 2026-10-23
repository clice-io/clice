module;

#include "support/prelude.h"

module clice:syntax.completion;

import :command.search_config;
import :syntax.dependency_graph;
import :syntax.scan;
import :syntax.token;
import :vfs.dir_cache;
import :vfs.file_table;

namespace clice {

/// The name the cursor is completing, where the text tells: a header name,
/// or the name of an import or a module declaration.
struct CompletionName {
    enum class Kind : std::uint8_t {
        None,
        IncludeQuoted,
        IncludeAngled,
        Import,
        /// The name in an implementation unit's `module` declaration.
        ModuleDeclaration,
    };

    Kind kind = Kind::None;
    std::string prefix;

    /// What a chosen candidate replaces: the name under the cursor — of an
    /// include, its last path component — typed part and untyped rest.
    LocalSourceRange replace;

    /// The name ends in its delimiter already: a header name's `>` or `"`,
    /// an import's or a module declaration's semicolon.
    bool closed = false;
};

/// The name the cursor at `offset` is in: the header name of an #include
/// or a `__has_include`, or the name of an import or a module declaration.
/// Pure text parsing — no compiler state needed.
CompletionName completion_name(llvm::StringRef text,
                               std::uint32_t offset,
                               const clang::LangOptions& dialect);

/// Whether the code before `offset` ends in a member or scope access
/// operator (`.`, `->`, `::`) — outside a directive, the only trigger
/// characters worth a candidate build. A pack ellipsis is not a member
/// access. The cursor's logical line is lexed alone: one inside a block
/// comment reads as code, which costs no more than a build.
bool follows_access_operator(llvm::StringRef text,
                             std::uint32_t offset,
                             const clang::LangOptions& dialect);

/// A designator written in a braced initializer, by the field it starts
/// with.
struct WrittenDesignator {
    llvm::StringRef field;
    /// Whether it designates the field as a whole (`.inner = {}`) rather
    /// than a part of it (`.inner.x = 1`, `.items[0] = 1`).
    bool whole;
};

/// The designators the braced initializer around `offset` has already,
/// where the text before `offset` ends in a `.` starting a designator of
/// its own (`{ .width = 800, .`), in order. Empty anywhere else, after a
/// `.` continuing a designator (`.inner.`) too.
std::vector<WrittenDesignator> written_designators(llvm::StringRef text,
                                                   std::uint32_t offset,
                                                   const clang::LangOptions& dialect);

/// The names an implementation unit's module declaration can take that
/// start with `prefix`: the modules the graph's interfaces provide.
std::vector<std::string> complete_module_declaration(const DependencyGraph& graph,
                                                     llvm::StringRef prefix);

/// The names an `import` in the unit whose module declaration `unit`
/// scanned can take that start with `prefix`: the graph's other provided
/// modules, and the partitions of the unit's own module but its own,
/// spelled `:partition` — in an interface unit, its interface partitions
/// only.
std::vector<std::string> complete_module_import(const DependencyGraph& graph,
                                                llvm::StringRef prefix,
                                                const ScanResult& unit);

/// Entry in the include path completion result.
struct IncludeCandidate {
    std::string name;
    bool is_directory = false;

    /// Where the search found it, ascending: the includer's directory, the
    /// quoted then the angled search directories, the system ones the
    /// command names, last the toolchain's own; in search order within each.
    std::uint32_t order = 0;
};

/// Return header and directory names matching a prefix in the given search paths.
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

}  // namespace clice
