#pragma once

/// The inside of the code action layer: what an action reads, how the
/// table anchors it, and the floor every action stands on. An action is a
/// function from a selection to CodeAction values, computed to completion
/// against one AST; the table in code_action.cpp decides which actions a
/// selection is offered. Nothing here knows the index or the protocol.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "semantic/selection.h"

#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"

namespace clice::feature::action {

struct Context {
    CompilationUnitRef unit;
    LocalSourceRange selection;
    /// The node the action anchored on.
    const SelectionTree::Node& node;
    /// Whether the main file is a header: a definition then belongs in the
    /// host source (the DefineInHost request) as much as in the header.
    bool main_is_header;
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
void expand_macro(CompilationUnitRef unit,
                  LocalSourceRange selection,
                  std::vector<CodeAction>& out);

/// Anchored on an unresolved name the compiler diagnosed at the selection.
void add_include(CompilationUnitRef unit, LocalSourceRange selection, std::vector<CodeAction>& out);

/// The main-file byte range a source range spells; nullopt when either
/// end is inside a macro or another file.
std::optional<LocalSourceRange> main_range(CompilationUnitRef unit, clang::SourceRange range);

/// The text a file range spells, in whichever file it lies; nullopt for
/// macro locations and ranges crossing files.
std::optional<llvm::StringRef> spelled_text(CompilationUnitRef unit, clang::SourceRange range);

/// The line containing `offset`: its first byte, and the byte past its
/// newline (the content's end on the last line).
std::uint32_t line_begin(llvm::StringRef content, std::uint32_t offset);
std::uint32_t line_end(llvm::StringRef content, std::uint32_t offset);

/// The whitespace opening the line containing `offset`.
llvm::StringRef line_indent(llvm::StringRef content, std::uint32_t offset);

/// The qualifier ("ns::S::") spelling members of `target` from inside
/// `from`: the named contexts of `target` not enclosing `from`. Class
/// templates spell their parameters as arguments ("S<T>::").
std::string qualifier_at(const clang::DeclContext* target, const clang::DeclContext* from);

/// `decl` spelled from inside `from`: the qualifier of its context and its
/// name. Enumerators of unscoped enums live in the enum's context.
std::string name_at(const clang::NamedDecl* decl, const clang::DeclContext* from);

/// A type spelled fully qualified, minus the namespaces enclosing `from`:
/// valid at `from` (null: at any scope of the TU).
std::string type_name(clang::ASTContext& context,
                      clang::QualType type,
                      const clang::DeclContext* from = nullptr);

/// "template <...>" heads of the class templates enclosing `decl` up to
/// `from`, outermost first, one per line, the parameters spelled without
/// their defaults; empty when none.
std::string template_heads(CompilationUnitRef unit,
                           const clang::Decl* decl,
                           const clang::DeclContext* from);

/// One "template <...>" head, the parameters spelled without defaults,
/// followed by the requires-clause when the list has one.
std::string template_head(CompilationUnitRef unit, const clang::TemplateParameterList* params);

/// Where member declarations go into a class body: right before the
/// closing brace, on lines of the body's own indentation.
struct BodyInsertion {
    std::uint32_t offset;
    std::string indent;
    /// The brace shares its line with earlier tokens: the inserted lines
    /// need a newline in front.
    bool break_before;
};

std::optional<BodyInsertion> class_body_insertion(CompilationUnitRef unit,
                                                  const clang::CXXRecordDecl* record);

/// Whether members appended at the end of the class body are public.
bool ends_public(const clang::CXXRecordDecl* record);

/// Visit the main file's declarations at file scope — the translation
/// unit's and, descending, those of namespaces, linkage specifications and
/// export blocks — in source order.
void for_each_file_scope_decl(CompilationUnitRef unit,
                              llvm::function_ref<void(const clang::Decl*)> visit);

/// The definitions of `record`'s member functions written out of line at
/// file scope in the main file, in file order.
std::vector<const clang::FunctionDecl*> out_of_line_definitions(CompilationUnitRef unit,
                                                                const clang::CXXRecordDecl* record);

/// The function's outermost written declaration: the function template
/// when it is one.
const clang::Decl* written_declaration(const clang::FunctionDecl* decl);

}  // namespace clice::feature::action
