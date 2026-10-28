module;

#include "support/prelude.h"

module clice:feature.code_actions;

import :compile.compilation;
import :index.query;
import :index.site;
import :syntax.source_text;
import :syntax.token;
import :vfs.ids;
import :vfs.path;

namespace clice::feature {

/// One kind per refactoring, below the standard `refactor.rewrite` and
/// `refactor.inline`, so that a client can ask for one alone: an editor
/// binds a key to it. Fixes are all `quickfix`.
namespace action_kind {

constexpr inline std::string_view define_inline = "refactor.rewrite.define.inline";
constexpr inline std::string_view define_out_of_line = "refactor.rewrite.define.outOfLine";
constexpr inline std::string_view define_missing = "refactor.rewrite.define.missing";
constexpr inline std::string_view implement_pure_virtuals =
    "refactor.rewrite.implementPureVirtuals";
constexpr inline std::string_view memberwise_constructor =
    "refactor.rewrite.generateMemberwiseConstructor";
constexpr inline std::string_view populate_switch = "refactor.rewrite.populateSwitch";
constexpr inline std::string_view expand_deduced_type = "refactor.rewrite.expandDeducedType";
constexpr inline std::string_view reorder_definitions = "refactor.rewrite.reorderDefinitions";
constexpr inline std::string_view expand_macro = "refactor.inline.macro";

}  // namespace action_kind

/// The kinds the actions produce: the advertised capability, and what a
/// request's `only` filter is matched against.
constexpr inline std::array code_action_kinds = {
    protocol::CodeActionKind::QuickFix,
    action_kind::define_inline,
    action_kind::define_out_of_line,
    action_kind::define_missing,
    action_kind::implement_pure_virtuals,
    action_kind::memberwise_constructor,
    action_kind::populate_switch,
    action_kind::expand_deduced_type,
    action_kind::reorder_definitions,
    action_kind::expand_macro,
};

/// One definition the index vets: dropped when any source knows a
/// definition of its symbol — this TU proved nothing defines it here,
/// another TU may.
struct DefinitionPiece {
    std::uint64_t entity = 0;
    std::string text;
};

/// Definitions to insert into the focus file once the index vetted them:
/// the surviving pieces joined by blank lines replace `range`, wrapped in
/// `before` and `after`, formatted by whoever assembles them.
struct DefineRequest {
    LocalSourceRange range;
    std::string before;
    std::string after;
    std::vector<DefinitionPiece> pieces;
};

/// Definitions to insert into the file's host source once the index
/// vetted them: after the definitions of `container`'s members there, at
/// its end when it has none (a container of 0 goes to the end outright).
/// The texts are fully qualified, valid at namespace scope anywhere in
/// the host.
struct DefineInHostRequest {
    std::uint64_t container = 0;
    std::vector<DefinitionPiece> pieces;
};

/// Where a new `#include` line goes in the focus file. A module unit
/// without a global module fragment gets one opened there.
struct IncludeInsertion {
    std::uint32_t offset = 0;
    bool opens_fragment = false;

    /// The text inserting `#include <header>`, the header spelled with its
    /// quotes or brackets.
    std::string text(llvm::StringRef header) const {
        return std::format("{}#include {}\n", opens_fragment ? "module;\n" : "", header);
    }
};

/// One action per header declaring `name` under `scope` ("std::" style,
/// empty for an unqualified name), each inserting its include directive
/// at `insertion` of the focus file.
struct IncludeRequest {
    std::string scope;
    std::string name;
    IncludeInsertion insertion;
};

/// What only the project index can decide: resolve_actions resolves it.
using IndexRequest = std::variant<DefineRequest, DefineInHostRequest, IncludeRequest>;

struct CodeAction {
    std::string title;
    protocol::CodeActionKind kind;
    std::vector<TextReplacement> edits;
    std::optional<IndexRequest> index;
};

/// The code actions available on a selection of the focus file. Every
/// action is computed to completion against this AST: single-TU actions
/// carry their edits, the ones needing the index carry their request.
auto code_actions(CompilationUnit& unit, LocalSourceRange selection) -> std::vector<CodeAction>;

/// Whether a request's kind filter (LSP CodeActionContext.only) admits
/// `kind`: LSP kinds are hierarchical, a requested "refactor" admits
/// "refactor.rewrite"; an empty filter admits every kind.
bool admits(llvm::ArrayRef<protocol::CodeActionKind> only, llvm::StringRef kind);

/// Whether `only` admits any kind of code_action_kinds: when it admits
/// none, there is nothing to compute.
bool admits_any(llvm::ArrayRef<protocol::CodeActionKind> only);

/// What the index requests of code actions are resolved against: the
/// project's index, the focus file, and the host source a header's
/// definitions go to.
struct ResolveInputs {
    /// Null resolves as an empty index would: every definition is kept and
    /// no header declares a name.
    const index::IndexQuery* query = nullptr;

    /// The focus file, the path whose clang-format style formats the
    /// definitions placed in it, and its text.
    Fid file;
    llvm::StringRef path;
    llvm::StringRef content;

    /// The host source; invalid when the file has none.
    Fid host;

    /// The host's path as the user knows it, which names it in the
    /// action's title.
    llvm::StringRef host_path;

    /// The path whose clang-format style formats the definitions placed
    /// in the host.
    llvm::StringRef host_style_path;

    /// The host's text, read only when the index places nothing there.
    llvm::function_ref<SourceText*()> host_text;

    /// The argument an `#include` of `header` takes in the focus file;
    /// nullopt when no search path reaches it.
    llvm::function_ref<std::optional<std::string>(CanonicalRef header)> include_argument;
};

/// Definitions inserted into the host source at `at`, a point of its text.
struct HostInsertion {
    index::LineColumn at;
    std::string text;
};

/// A code action with its index request resolved: it edits the focus
/// file, inserts into the host, or holds definitions no host takes.
struct ResolvedAction {
    std::string title;
    protocol::CodeActionKind kind;

    /// The edits of the focus file, at bytes of its text.
    std::vector<TextReplacement> edits;

    std::optional<HostInsertion> host;

    /// The definitions of a DefineInHostRequest the index keeps, when the
    /// file has no host to place them in.
    std::vector<std::string> host_definitions;
};

/// The actions `only` admits, their index requests resolved. A
/// definition another source defines already is dropped, an action whose
/// definitions all are goes; definitions for the host go after the
/// definitions of their container's members there, else at its end; an
/// IncludeRequest becomes one action per header declaring the name.
auto resolve_actions(std::vector<CodeAction> actions,
                     llvm::ArrayRef<protocol::CodeActionKind> only,
                     const ResolveInputs& inputs) -> std::vector<ResolvedAction>;

/// The LSP form of resolved actions: the focus file's edits placed in the
/// text `positions` maps, each file's edit naming it as `focus` and `host`
/// do. An action holding definitions no host takes is left out.
auto to_protocol(std::vector<ResolvedAction> actions,
                 const PositionMap& positions,
                 PositionEncoding encoding,
                 const protocol::OptionalVersionedTextDocumentIdentifier& focus,
                 const protocol::OptionalVersionedTextDocumentIdentifier& host)
    -> std::vector<protocol::CodeAction>;

}  // namespace clice::feature
