module;

#include "support/prelude.h"

/// The inside of the code action layer: what an action reads, how the
/// table anchors it, and the floor every action stands on. An action is a
/// function from a selection to CodeAction values, computed to completion
/// against one AST; the table in code_actions.cpp decides which actions a
/// selection is offered. Nothing here knows the index or the protocol.

module clice:feature.code_actions.action;

import :feature.code_actions;
import :semantic.selection;
import :support.text;

namespace clice::feature::action {

struct Context {
    CompilationUnit& unit;
    /// The node the action anchored on.
    const SelectionTree::Node& node;
    /// Whether the focus file is a header: a definition then belongs in the
    /// host source (the DefineInHostRequest) as much as in the header.
    bool focus_is_header;
};

using Run = void (*)(const Context& ctx, std::vector<CodeAction>& out);

enum class Reach : std::uint8_t {
    /// The node itself must be the innermost node the selection covers.
    Innermost,
    /// Any node from the innermost covered one up to the root.
    Ancestors,
};

struct Spec {
    clang::ASTNodeKind anchor;
    Reach reach;
    Run run;
    /// Reformat the edits with the file's clang-format style.
    bool format;
};

void define(const Context& ctx, std::vector<CodeAction>& out);
void define_missing(const Context& ctx, std::vector<CodeAction>& out);
void implement_pure_virtuals(const Context& ctx, std::vector<CodeAction>& out);
void memberwise_constructor(const Context& ctx, std::vector<CodeAction>& out);
void populate_switch(const Context& ctx, std::vector<CodeAction>& out);
void expand_deduced_type(const Context& ctx, std::vector<CodeAction>& out);
void reorder_definitions(const Context& ctx, std::vector<CodeAction>& out);

/// Anchored on the macro expansion under the selection, not on the tree.
void expand_macro(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out);

/// Anchored on an unresolved name the compiler diagnosed at the selection.
void add_include(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out);

/// The fixes the diagnostics on the selection carry, the compiler's and
/// clang-tidy's: each diagnostic's own and those of its notes.
void fix_its(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out);

IncludeInsertion include_insertion(CompilationUnit& unit);

/// Where a new `import` line goes in `content`: after the last import
/// ahead of every other declaration, else after the module declaration;
/// in a file with neither, after its leading directives.
std::uint32_t import_insertion(llvm::StringRef content, const clang::LangOptions& options);

/// The focus-file text a range spells when neither end is in a macro.
/// Code actions edit only such text: a macro argument may be expanded
/// more than once, a comma an edit brings into it splits it, and the
/// macro's own tokens around it change what the edit means.
std::optional<LocalSourceRange> editable_range(CompilationUnit& unit, clang::SourceRange range);

/// The text a source range spells when neither end is in a macro, in
/// whichever file it lies but a builtin buffer.
std::optional<llvm::StringRef> spelled_text(CompilationUnit& unit, clang::SourceRange range);

/// The whitespace opening the line containing `offset`.
llvm::StringRef line_indent(llvm::StringRef content, std::uint32_t offset);

bool is_cv(TokenKind kind);

/// Whether a context is a file scope a definition is written at: the
/// translation unit, a namespace, a linkage specification or an export
/// block.
bool at_file_scope(const clang::DeclContext* context);

/// The qualifier ("ns::S::") spelling members of `target` from inside
/// `from`: the named contexts of `target` not enclosing `from`. Class
/// templates spell their parameters as arguments ("S<T>::").
std::string qualifier_at(const clang::DeclContext* target, const clang::DeclContext* from);

/// A name written in C++ text clang printed, a type's: its components as
/// `::` joins them.
struct PrintedName {
    LocalSourceRange range;
    llvm::SmallVector<llvm::StringRef, 4> components;

    /// It comes after `::`, `.` or `->`: a component of a name or an
    /// expression already begun.
    bool continues = false;
};

/// The names `text` writes, lexed in `dialect`; a literal's text names
/// nothing.
std::vector<PrintedName> printed_names(llvm::StringRef text, const clang::LangOptions& dialect);

/// A type spelled for `from`, declaring `name` when one is given
/// ("int (*name)(int)"): each name drops the namespaces enclosing `from`
/// as far as it still finds its entity there, and the compiler's internal
/// names become the standard ones `from` sees declared before `at`, which
/// is no later than where the text goes. Nullopt for a type `from` cannot
/// name: a lambda or unnamed type, another function's local type, a member
/// type the text has no access to. The text has the access of `access`,
/// `from` when null: an out-of-line member definition has its class's.
std::optional<std::string> type_name(clang::ASTContext& context,
                                     clang::QualType type,
                                     const clang::DeclContext* from,
                                     clang::SourceLocation at,
                                     llvm::StringRef name = {},
                                     const clang::DeclContext* access = nullptr);

/// "template <...>" heads of the class templates enclosing `decl` up to
/// `from`, outermost first, one per line, the parameters spelled without
/// their defaults; empty when none.
std::string template_heads(CompilationUnit& unit,
                           const clang::Decl* decl,
                           const clang::DeclContext* from);

/// A member declaration of one line and the access it is declared with.
struct Member {
    clang::AccessSpecifier access;
    std::string line;
};

/// `members` appended to the class body in their order, on the body's own
/// indentation, under an access label wherever the access changes from
/// the one the body ends in; nullopt when the class head or brace is
/// spelled by a macro.
std::optional<TextReplacement> insert_members(CompilationUnit& unit,
                                              const clang::CXXRecordDecl* record,
                                              llvm::ArrayRef<Member> members);

/// Visit the focus file's declarations at file scope — the translation
/// unit's and, descending, those of namespaces, linkage specifications and
/// export blocks — in source order.
void for_each_file_scope_decl(CompilationUnit& unit,
                              llvm::function_ref<void(const clang::Decl*)> visit);

/// The definitions of `record`'s member functions written out of line at
/// file scope in the focus file, in file order.
std::vector<const clang::FunctionDecl*> out_of_line_definitions(CompilationUnit& unit,
                                                                const clang::CXXRecordDecl* record);

/// The function's outermost written declaration: the function template
/// when it is one.
const clang::Decl* written_declaration(const clang::FunctionDecl* decl);

}  // namespace clice::feature::action
