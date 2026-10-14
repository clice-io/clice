module;

#include "modules/prelude.h"

module clice:semantic.display;

/// The rendering side of the AST helpers: everything that turns an AST
/// entity into a human-facing string lives in this namespace, so that
/// clang printing quirks are patched in exactly one place. Semantic
/// queries stay in decls.h and types.h.
namespace clice::display {

class ReadableTypes;

/// How the policy-driven renderers below print. The boolean fields select
/// PrintingPolicy flags; the quirk fields gate the patches applied on top of
/// clang's printers. A feature keeps one Options for a request, so every
/// type it shows prints the same way.
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

    /// Print a reserved parameter name without its uglification (`__x` as
    /// `x`).
    bool clean_uglified = false;

    /// type(): print the type as a reader knows it (ReadableTypes), through
    /// the request's collection of standard aliases.
    ReadableTypes* readable = nullptr;

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

    /// definition(): how many members, at most, the body of a class or enum
    /// definition lists — nested ones count too; 0 prints no body.
    std::uint32_t max_members = 0;
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

    /// The names its declarations write for it (decls::named_parameters), as
    /// written: a documentation comment names it by one of them.
    std::vector<std::string> written;

    /// std::nullopt if no default is provided.
    std::optional<std::string> default_value;

    /// A function parameter as its declaration writes it (parameter()), which
    /// is how it prints; a template parameter prints from the fields above.
    std::optional<std::string> declaration;
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

/// The template arguments of a template specialization as written in the
/// source code ("<int, 4>"). Empty if the decl is not a specialization.
auto template_args(const clang::NamedDecl& decl) -> std::string;

/// A short human-readable summary of an expression, e.g. for block-end
/// hints: names, literals, and simple unary/binary operator forms.
auto summarize(const clang::Expr* expr) -> std::string;

/// The class(es) and function a local declaration lives in, joined with
/// "::". Empty if the decl is not local.
auto local_scope(const clang::Decl* decl) -> std::string;

/// The namespace containing the declaration, empty for the global
/// namespace.
auto namespace_scope(const clang::Decl* decl) -> std::string;

/// The class scope type() writes back in front of `type` when the options
/// suppress scopes. Class scopes carry meaning, but SuppressScope also drops
/// two of them: the computed scope of a canonical tag (a deduced `auto`
/// prints as its deduced, canonical type) and the written qualifier of a
/// template-id. Every other node (typedefs, dependent names, written tag
/// types) prints its written qualifier regardless of the policy — restoring
/// theirs would duplicate it. Null when nothing is restored.
auto restored_scope(clang::QualType type, const Options& options) -> clang::NestedNameSpecifier;

/// The printing policy the options select: the context's policy with the
/// option-selected flags applied on top.
auto printing_policy(clang::ASTContext& context, const Options& options) -> clang::PrintingPolicy;

/// Pretty-print a type, applying the option-gated quirks and the optional
/// aka form; a `declarator` (a name, `...name`) goes where the type puts it,
/// as in `void (*name)(int)`.
auto type(clang::ASTContext& context,
          clang::QualType type,
          const Options& options = {},
          llvm::StringRef declarator = {}) -> Type;

/// Apply a series of heuristic methods to determine whether or not the
/// type is suitable for desugaring (e.g. getting the real name behind the
/// using-alias name). If so, return the desugared type. Otherwise, return
/// the unchanged type.
///
/// This could be refined further. See
/// https://github.com/clangd/clangd/issues/1298.
auto maybe_desugar(clang::ASTContext& context, clang::QualType type) -> clang::QualType;

/// Types as a reader knows them: a deduced `auto` gives way to the type it
/// stands for, an alias the standard library keeps to itself
/// (`_NonArray<int>`) to what it names, and a standard class template specialization to the
/// standard alias naming it (`string` for `basic_string<char>`), in template arguments too, under
/// the namespaces the code writes it with
/// (`std::string`) unless the options suppress scopes. libc++ marks those
/// aliases as preferred names, which the printer honors; libstdc++ only
/// declares them. One per request: a specialization's alias is looked up
/// once, on first use.
class ReadableTypes {
public:
    ReadableTypes(clang::ASTContext& context, const Options& options) :
        context(context), scoped(!options.suppress_scope) {}

    auto readable(clang::QualType type) -> clang::QualType;

private:
    auto readable(const clang::TemplateArgument& argument) -> clang::TemplateArgument;

    bool reserved(const clang::NamedDecl* decl) const;

    /// A reserved name a library keeps to itself, within namespace std or a
    /// reserved one (`_NonArray`, `__gnu_cxx::__alloc_traits`). One outside
    /// them, as `__m128`, is the name its header gives its users.
    bool internal(const clang::NamedDecl* decl) const;

    /// The scope the code names a namespace member through, `std::pmr::`.
    auto written_scope(const clang::NamedDecl* decl) const -> clang::NestedNameSpecifier;

    /// The alias the standard names a specialization of its class templates
    /// with, looked up by name; null when it gives none, or two.
    auto standard_alias(const clang::ClassTemplateSpecializationDecl& spec)
        -> const clang::TypedefNameDecl*;

    clang::ASTContext& context;
    bool scoped;
    llvm::DenseMap<const clang::Type*, const clang::TypedefNameDecl*> aliases;
};

/// Evaluate an expression and render its value: enums symbolically, big
/// or negative integers with a hex form attached. nullopt when the
/// expression is dependent, not evaluatable, or of a type whose value is
/// not meaningful (void, functions, records).
auto expr_value(const clang::ASTContext& context, const clang::Expr* expr)
    -> std::optional<std::string>;

/// The default argument of `param` as the source writes it, on one line;
/// nullopt when there is none, or it is not parsed yet.
auto default_argument(const clang::ParmVarDecl* param, const Options& options = {})
    -> std::optional<std::string>;

/// The name `function`'s parameter `index` shows under: the first one its
/// declarations write (decls::named_parameters), a reserved name without
/// its uglification (`__x` as `x`); empty when none names it.
auto parameter_name(const clang::FunctionDecl& function, unsigned index) -> llvm::StringRef;

/// What a member function's declaration writes after its parameters:
/// ` const`, ` &`, ` &&`; empty for none.
auto method_qualifiers(const clang::CXXMethodDecl& method) -> std::string;

/// Render a function parameter with its default value, under the first name
/// its declarations write (decls::named_parameters).
auto param(const clang::ParmVarDecl* param, const Options& options = {}) -> Param;

/// A function parameter as its declaration writes it, under `name`: the
/// type as written (an array does not decay), a pack's `...`, then its
/// default argument.
auto parameter(const clang::ParmVarDecl& param, llvm::StringRef name, const Options& options)
    -> std::string;

/// A field as its declaration writes it: `mutable`, then its bit width and
/// default member initializer as the source writes them, as parameter()
/// gives a default argument.
auto field(const clang::FieldDecl& field, const Options& options) -> std::string;

/// Render every parameter of a template parameter list.
auto template_params(const clang::TemplateParameterList* params, const Options& options = {})
    -> std::vector<Param>;

/// The rendered "type" of a single template parameter: "typename"/"class"
/// for type parameters, the declared type for non-type parameters and
/// "template <...> class" for template template parameters.
auto template_param_type(const clang::NamedDecl* param, const Options& options = {}) -> Type;

/// Pretty-print the declaration itself. A class or enum with a visible
/// definition prints that definition, its body summarized as
/// options.max_members asks. Initializers longer than 200 tokens, counted by
/// `token_count` over a range, are suppressed — such lists are not useful in
/// a hover card and are catastrophically expensive to print.
auto definition(const clang::Decl* decl,
                const Options& options,
                llvm::function_ref<std::size_t(clang::SourceRange)> token_count) -> std::string;

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Type& type);

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Param& param);

}  // namespace clice::display
