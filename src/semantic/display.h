#pragma once

#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"

namespace clang::syntax {

class TokenBuffer;

}

/// The rendering side of the AST helpers: everything that turns an AST
/// entity into a human-facing string lives in this namespace, so that
/// clang printing quirks are patched in exactly one place. Semantic
/// queries stay in decls.h and types.h.
namespace clice::display {

/// How the policy-driven renderers below print. The boolean fields mirror
/// the PrintingPolicy configurations the features historically used; the
/// quirk fields gate the patches we apply on top of clang's printers.
/// Divergent defaults are kept per feature for now — unifying them is a
/// deliberate, snapshot-reviewed change, not a side effect of this module.
struct Options {
    /// Drop scope qualifiers to keep names short.
    bool suppress_scope = false;

    /// Skip unwritten scopes (inline/anonymous namespaces) in qualifiers.
    bool suppress_unwritten_scope = false;

    /// Print declarations without bodies or long initializers.
    bool terse = false;

    /// Print as a declaration rather than mirroring the AST node.
    bool polish_for_declaration = false;

    /// Print constant expressions as written, not their evaluated value.
    bool constants_as_written = false;

    /// Don't print a class template's args as part of a constructor name.
    bool suppress_ctor_template_args = false;

    /// "(anonymous struct at foo.cc:1:2)" instead of "(anonymous struct)".
    /// Nobody renders locations on purpose; only the untouched policy of
    /// signature help keeps them today.
    bool anonymous_tag_locations = false;

    /// name_of(): include the written nested name qualifier and the
    /// template specialization arguments ("Foo::bar<int>" instead of
    /// "bar"), and present nameless entities ("(anonymous struct)",
    /// "(lambda)", "using namespace ns") instead of leaving them empty.
    bool qualified = true;

    /// type(): resolve decltype(...) to its underlying type before
    /// printing, since TypePrinter does not.
    bool resolve_decltype = false;

    /// type(): prefix an unqualified outer tag type with its tag keyword
    /// ("struct S") for extra clarity.
    bool tag_keyword_prefix = false;

    /// type(): attach the desugared form as `aka` when it differs.
    bool show_aka = false;
};

/// A pretty-printed type, optionally with its desugared form.
struct Type {
    Type() = default;

    Type(llvm::StringRef text) : text(text.str()) {}

    Type(llvm::StringRef text, llvm::StringRef aka) : text(text.str()), aka(aka.str()) {}

    /// Allow assigning string literals to std::optional<Type>, which
    /// would otherwise require two implicit user conversions.
    Type(const char* text) : Type(llvm::StringRef(text)) {}

    Type(const char* text, const char* aka) : Type(llvm::StringRef(text), llvm::StringRef(aka)) {}

    bool operator==(const Type&) const = default;

    /// Pretty-printed type.
    std::string text;

    /// Desugared type.
    std::optional<std::string> aka;
};

/// A rendered parameter of a function or a template. For example:
/// - void foo(ParamType Name = DefaultValue)
/// - template <ParamType Name = DefaultType> class Foo {};
struct Param {
    bool operator==(const Param&) const = default;

    /// The printable parameter type, e.g. "int", or "typename" (in
    /// template parameters).
    std::optional<Type> type;

    /// std::nullopt for unnamed parameters.
    std::optional<std::string> name;

    /// std::nullopt if no default is provided.
    std::optional<std::string> default_value;
};

/// The raw spelled identifier of the decl, empty if its name is not a
/// simple identifier (operators, special names, anonymous entities).
auto identifier_of(const clang::NamedDecl* decl) -> llvm::StringRef;

/// If the expr spells a single unqualified identifier, return it. Empty
/// otherwise.
auto identifier_of(const clang::Expr* expr) -> llvm::StringRef;

/// The synthesized human-facing name of the decl:
/// constructors/destructors/conversions print their type, operators their
/// spelling. With options.qualified (the default) the written nested name
/// qualifier and template specialization arguments are included and
/// nameless entities get a presentation ("(anonymous struct)", "(lambda)",
/// "using namespace ns"); without it the bare name is rendered and
/// nameless entities yield an empty string.
auto name_of(const clang::NamedDecl* decl, const Options& options = {}) -> std::string;

/// A short human-readable summary of an expression, e.g. for block-end
/// hints: names, literals, and simple unary/binary operator forms.
auto summarize(const clang::Expr* expr) -> std::string;

/// The formatted documentation comment attached to the decl, empty if
/// none. Namespaces are skipped as their comments are rarely useful.
auto comment(const clang::NamedDecl* decl) -> std::string;

/// The class(es) and function a local declaration lives in, joined with
/// "::". Empty if the decl is not local.
auto local_scope(const clang::Decl* decl) -> std::string;

/// The namespace containing the declaration, empty for the global
/// namespace.
auto namespace_scope(const clang::Decl* decl) -> std::string;

/// Pretty-print a type, applying the option-gated quirks and the optional
/// aka form.
auto type(clang::ASTContext& context, clang::QualType type, const Options& options = {}) -> Type;

/// Apply a series of heuristic methods to determine whether or not the
/// type is suitable for desugaring (e.g. getting the real name behind the
/// using-alias name). If so, return the desugared type. Otherwise, return
/// the unchanged type.
///
/// This could be refined further. See
/// https://github.com/clangd/clangd/issues/1298.
auto maybe_desugar(clang::ASTContext& context, clang::QualType type) -> clang::QualType;

/// Evaluate an expression and render its value: enums symbolically, big
/// or negative integers with a hex form attached. nullopt when the
/// expression is dependent, not evaluatable, or of a type whose value is
/// not meaningful (void, functions, records).
auto expr_value(const clang::ASTContext& context, const clang::Expr* expr)
    -> std::optional<std::string>;

/// Render a function parameter with its default value.
auto param(const clang::ParmVarDecl* param, const Options& options = {}) -> Param;

/// Render every parameter of a template parameter list.
auto template_params(const clang::TemplateParameterList* params, const Options& options = {})
    -> std::vector<Param>;

/// The rendered "type" of a single template parameter: "typename"/"class"
/// for type parameters, the declared type for non-type parameters and
/// "template <...> class" for template template parameters.
auto template_param_type(const clang::NamedDecl* param, const Options& options = {}) -> Type;

/// Pretty-print the declaration itself. When a token buffer is given,
/// initializers longer than 200 tokens are suppressed — such lists are
/// not useful in a hover card and are catastrophically expensive to
/// print.
auto definition(const clang::Decl* decl,
                const Options& options = {},
                const clang::syntax::TokenBuffer* tb = nullptr) -> std::string;

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Type& type);

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Param& param);

}  // namespace clice::display
