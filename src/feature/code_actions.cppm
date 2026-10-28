module;

#include "support/prelude.h"

module clice:feature.code_actions;

import :compile.compilation;
import :index.query;
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

/// What only the project index can decide: resolved by the master at the
/// reply edge; clice inspect resolves it as an empty index would.
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

/// The pieces the index keeps, joined by blank lines; nullopt when none
/// survives.
auto assemble_definitions(llvm::ArrayRef<DefinitionPiece> pieces,
                          llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::string>;

/// Whether a request's kind filter (LSP CodeActionContext.only) admits
/// `kind`: LSP kinds are hierarchical, a requested "refactor" admits
/// "refactor.rewrite"; an empty filter admits every kind.
bool admits(llvm::ArrayRef<protocol::CodeActionKind> only, llvm::StringRef kind);

/// One file an action edits, its edits in LSP form.
struct FileEdit {
    std::string uri;
    std::optional<int> version;
    std::vector<protocol::TextEdit> edits;
};

protocol::CodeAction to_protocol(std::string title, protocol::CodeActionKind kind, FileEdit file);

/// A DefineRequest's edit of the focus file `file`, whose text is
/// `content`: the pieces the index keeps (assemble_definitions) formatted
/// in place; none when none survives.
auto define_edits(const DefineRequest& request,
                  llvm::StringRef file,
                  llvm::StringRef content,
                  llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::vector<TextReplacement>>;

/// The edit inserting a DefineInHostRequest's definitions, `text`
/// formatted for the host file `host`: after the last definition of the
/// container's members in the host's serving index, in the coordinates
/// that source guarantees — the relations read directly, as an open host's
/// own index knows its definitions before the background index merges them
/// into the project index — else at the end of the host's text, which
/// `host_text` reads only then; none when it cannot.
auto host_edit(const index::IndexQuery& query,
               Fid host,
               std::uint64_t container,
               llvm::StringRef text,
               llvm::function_ref<SourceText*()> host_text,
               PositionEncoding encoding) -> std::optional<protocol::TextEdit>;

/// The headers declaring the name an IncludeRequest names, as the index
/// knows them: the files of the declarations and definitions of the
/// symbols spelling it exactly in its scope, each once; the focus file
/// `file`, sources and context headers left out.
auto declaring_headers(const index::IndexQuery& query, const IncludeRequest& request, Fid file)
    -> std::vector<CanonicalRef>;

}  // namespace clice::feature
