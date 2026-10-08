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

struct SearchConfig;

/// What kind of preamble-level completion is being requested.
enum class CompletionContext : std::uint8_t {
    None,
    IncludeQuoted,
    IncludeAngled,
    Import,
};

/// Result of detecting the completion context from source text.
struct PreambleCompletionContext {
    CompletionContext kind = CompletionContext::None;
    std::string prefix;

    /// What a chosen candidate replaces: the name under the cursor — of an
    /// include, its last path component — typed part and untyped rest.
    LocalSourceRange replace;

    /// An import statement that ends in its semicolon already.
    bool closed = false;
};

/// Detect whether the cursor is inside a #include or import directive.
/// Pure text parsing — no compiler state needed.
PreambleCompletionContext detect_completion_context(llvm::StringRef text, std::uint32_t offset);

/// Whether the text before `offset` ends in a member or scope access
/// operator (`.`, `->`, `::`) — outside a directive, the only trigger
/// characters worth a candidate build. A pack ellipsis is not a member
/// access.
bool follows_access_operator(llvm::StringRef text, std::uint32_t offset);

/// The names an `import` in a unit of `module_name` (empty outside a
/// module unit) can take that start with `prefix`: the graph's other
/// provided modules, and the partitions of the unit's own module but its
/// own, spelled `:partition`.
std::vector<std::string> complete_module_import(const DependencyGraph& graph,
                                                llvm::StringRef prefix,
                                                llvm::StringRef module_name);

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
