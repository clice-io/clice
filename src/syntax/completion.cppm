module;

#include "modules/prelude.h"

module clice:syntax.completion;

import :syntax.token;
import :vfs.file_table;

namespace clice {

namespace vfs {

struct Scope;

}

class DependencyGraph;

struct ScanResult;

struct SearchConfig;

/// What kind of preamble-level completion is being requested.
enum class CompletionContext : std::uint8_t {
    None,
    IncludeQuoted,
    IncludeAngled,
    Import,
    /// The name in an implementation unit's `module` declaration.
    ModuleDeclaration,
};

/// Result of detecting the completion context from source text.
struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;

    /// What a chosen candidate replaces: the name under the cursor — of an
    /// include, its last path component — typed part and untyped rest.
    LocalSourceRange replace;

    /// The name ends in its delimiter already: a header name's `>` or `"`,
    /// an import's or a module declaration's semicolon.
    bool closed = false;
};

/// Detect whether the cursor is in the header name of an #include or a
/// `__has_include`, or in the name of an import or a module declaration.
/// Pure text parsing — no compiler state needed.
PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset);

/// Whether the text before `offset` ends in a member or scope access
/// operator (`.`, `->`, `::`) — outside a directive, the only trigger
/// characters worth a candidate build. A pack ellipsis is not a member
/// access.
bool follows_access_operator(llvm::StringRef text, std::uint32_t offset);

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
std::vector<WrittenDesignator> written_designators(llvm::StringRef text, std::uint32_t offset);

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
