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
    /// Drop scope qualifiers to keep names short. Through options.readable,
    /// a type keeps only the namespaces inside std, whose names repeat those
    /// of std (`pmr::string`, `chrono::seconds`).
    bool suppress_scope = false;

    /// type(), through options.readable, as a hint shows a type: a member
    /// alias of a class template specialization gives way to what it names
    /// where that reads shorter and names nothing the library keeps to
    /// itself (`size_t` for `vector<int>::size_type`, but
    /// `vector<int>::iterator`); a type keeps the namespaces code at the
    /// scope would write but `std` and those a using-directive brings in
    /// (`chrono::seconds`; `StringRef` under `using namespace llvm`).
    bool hint_types = false;

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

    /// type(): attach the desugared form as `aka` when it differs.
    bool show_aka = false;

    /// type(): name a class template inside it as code there does, `Box`,
    /// not by its own specialization (`Box<T>`, whose unnamed parameters
    /// print canonically, `value-parameter-0-2`).
    bool injected_as_written = false;

    /// definition(): how many members, at most, the body of a class or enum
    /// definition lists — nested ones count too; 0 prints no body.
    std::uint32_t max_members = 0;

    /// type(): the scope the type is shown in. Through options.readable, a
    /// type named in another scope is named as code here names it
    /// (`std::vector<int>::iterator`, not the `iterator` the vector writes);
    /// without one, as the translation unit names it. A declaration's types
    /// are shown in its scope, a deduced type or an expression's where its
    /// code is.
    const clang::DeclContext* scope = nullptr;

    /// type(): the expression the type is read from — what a deduced type is
    /// deduced from, an expression shown with its own — which names the
    /// specialization a member typedef of its class template is read through
    /// (ReadableTypes::readable).
    const clang::Expr* source = nullptr;

    /// type(): the dependent specialization of a class template a member of
    /// it is shown through, as the code names it (`vector<Point<T>>` for
    /// `pts.push_back`): the template's own parameters read as its
    /// arguments.
    const clang::TemplateSpecializationType* specialization = nullptr;
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

/// The name a declaration goes by in the index and the outline: its bare
/// name, a constructor's without its class template's arguments, a
/// deduction guide's label (`<deduction guide for Box>`); a nameless
/// entity's presentation, `unnamed` set.
struct SymbolName {
    std::string text;
    bool unnamed = false;
};

auto symbol_name(const clang::NamedDecl* decl) -> SymbolName;

/// The template arguments of a template specialization as written in the
/// source code ("<int, 4>"). Empty if the decl is not a specialization.
auto template_args(const clang::NamedDecl& decl) -> std::string;

/// The arguments of a class or variable template specialization as a reader
/// names them: through options.readable, the ones left to their defaults
/// omitted (`<std::string>` for `vector<string, allocator<string>>`); a
/// partial specialization's as written. Empty for any other declaration.
auto specialization_args(const clang::NamedDecl& decl, const Options& options) -> std::string;

/// A short human-readable summary of an expression, e.g. for block-end
/// hints: names, literals, and simple unary/binary operator forms.
auto summarize(const clang::Expr* expr) -> std::string;

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

/// Types as a reader knows them: a deduced `auto` gives way to the type it
/// stands for; an alias the library keeps to itself (`_NonArray<int>`, a
/// member of `__alloc_traits`) or one that computes its type as a trait
/// does (`common_type<A, B>::type`, `enable_if_t<…>`) to what it names; a
/// standard class template specialization to the standard alias naming it
/// (`string` for `basic_string<char>`), and a class the library keeps to
/// itself to the member alias a class of its arguments or of the source
/// names it with (`vector<int>::iterator` for `__normal_iterator<…>`). In
/// template arguments too, every name under the qualifier code at the scope
/// would write. libc++ marks the standard aliases as preferred names, which
/// the printer honors; libstdc++ only declares them. One per request: a
/// specialization's alias is looked up once, on first use.
class ReadableTypes {
public:
    ReadableTypes(clang::ASTContext& context, const Options& options) :
        context(context), policy(printing_policy(context, options)),
        scoped(!options.suppress_scope), hinted(options.hint_types) {}

    /// `type` as a reader knows it, shown in `scope`: a type named in
    /// another scope gets the qualifier that names it from there, the
    /// translation unit without a scope. A member typedef of a class template
    /// that names the same type in every specialization stays the template's
    /// own in a specialization's members (`size_type` of `vector<int>::size()`):
    /// it is named through the specialization a member `source` names belongs
    /// to, by its name alone without one; a member of a base class through
    /// the derived class `source` reaches it in (`DenseMap<K, V>::iterator`
    /// of `map.find(key)`, `find` being `DenseMapBase`'s). The parameters of
    /// the template a dependent `specialization` names read as its
    /// arguments (Options::specialization).
    auto readable(clang::QualType type,
                  const clang::DeclContext* scope,
                  const clang::Expr* source = nullptr,
                  const clang::TemplateSpecializationType* specialization = nullptr)
        -> clang::QualType;

    /// What `type` is past every alias, its parts named as readable() names
    /// them but a class the library keeps to itself left as itself: an aka
    /// tells what the alias hides (`__normal_iterator<…>` for
    /// `vector<int>::iterator`).
    auto underlying(clang::QualType type,
                    const clang::DeclContext* scope,
                    const clang::TemplateSpecializationType* specialization) -> clang::QualType;

    /// A name a library keeps to itself: a reserved one, or a member of a
    /// class or namespace with one, within namespace std or a reserved one
    /// (`_NonArray`, `__gnu_cxx::__alloc_traits<A>::value_type`). One
    /// outside them, as `__m128`, is the name its header gives its users.
    bool internal(const clang::NamedDecl* decl) const;

    /// Whether `type`, a readable one, is a class or alias the library keeps
    /// to itself, or a pointer, reference or array of one: no name a reader
    /// knows spells it.
    bool internal(clang::QualType type) const;

private:
    /// Whose aliases a type carries, which decides those it keeps.
    enum class Sugar : std::uint8_t {
        /// The code's, as a declaration writes it: every alias stays but
        /// one the library keeps to itself.
        Written,
        /// What a deduction got from the declarations it read: an alias
        /// that computes its type (`invoke_result_t<…>`) gives way to it.
        Deduced,
        /// The library's own, past the aliases the code names a type with:
        /// a specialization's member alias also gives way to what it names
        /// where that reads shorter (Options::hint_types).
        Library,
    };

    auto readable(clang::QualType type,
                  const clang::DeclContext* scope,
                  const clang::Expr* source,
                  Sugar sugar) -> clang::QualType;

    auto readable(const clang::TemplateArgument& argument,
                  const clang::DeclContext* scope,
                  const clang::Expr* source,
                  Sugar sugar) -> clang::TemplateArgument;

    /// The qualifier that names `decl`, a type's declaration, from `scope`;
    /// null when its name alone does.
    auto qualifier(const clang::NamedDecl& decl,
                   const clang::DeclContext* scope,
                   const clang::Expr* source) -> clang::NestedNameSpecifier;

    bool reserved(const clang::NamedDecl* decl) const;

    /// The argument `type`, a parameter of the template `substitution` names,
    /// reads as; null for any other type, or a parameter it gives none.
    auto argument_of(const clang::Type& type) const -> const clang::TemplateArgument*;

    /// Whether `type` names something the library keeps to itself anywhere,
    /// its qualifier and template arguments included.
    bool shows_internal(clang::QualType type) const;

    /// The typedef the code names the type of `sizeof` or of a pointer
    /// difference with (`std::size_t`, else `size_t`); null when it declares
    /// none.
    auto standard_name(const clang::PredefinedSugarType& type) -> const clang::TypedefNameDecl*;

    /// The alias the standard names a specialization of its class templates
    /// with, looked up by name; null when it gives none, or two.
    auto standard_alias(const clang::ClassTemplateSpecializationDecl& spec)
        -> const clang::TypedefNameDecl*;

    /// A public member alias naming `tag`, a class the library keeps to
    /// itself, in a class among its template arguments or the types of
    /// `source`'s expressions; null when none names it.
    auto public_alias(const clang::TagDecl& tag, const clang::Expr* source) const
        -> const clang::TypedefNameDecl*;

    clang::ASTContext& context;
    clang::PrintingPolicy policy;
    bool scoped;
    bool hinted;

    /// Whether a class the library keeps to itself gives way to the alias
    /// naming it, and a standard class to its standard alias; not while
    /// underlying() names what a type is.
    bool aliasing = true;

    /// The dependent specialization whose template's parameters the call in
    /// progress reads as its arguments.
    const clang::TemplateSpecializationType* substitution = nullptr;

    /// The classes the call in progress names by a public alias.
    llvm::SmallPtrSet<const clang::TagDecl*, 4> naming;

    /// The alias found for a specialization or a compiler's internal type,
    /// null when there is none.
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

/// The parameters a call of `function` passes arguments to, as every feature
/// shows them — under their names, with their defaults — then the
/// function's qualifiers: `(int x, float y = 1) const`. An explicit object
/// parameter is the object the call is made on, no argument; printed as a
/// declaration (options.polish_for_declaration), it leads the list as
/// written, `(this Widget& self)`.
auto call_signature(const clang::FunctionDecl& function, const Options& options) -> std::string;

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

/// The rendered "type" of a single template parameter: "typename"/"class"
/// for type parameters, the declared type for non-type parameters and
/// "template <...> class" for template template parameters.
auto template_param_type(const clang::NamedDecl* param, const Options& options = {}) -> Type;

/// An attribute specifier as the source writes it: `[[nodiscard]]`,
/// `__attribute__((packed))`, `alignas(8)`.
auto attribute(const clang::Attr& attr, const clang::ASTContext& context) -> std::string;

/// A macro a declaration writes attributes with, `NODISCARD`, and the
/// attributes it expands to, `[[nodiscard]]`.
struct AttributeMacro {
    std::string use;
    std::string attributes;
};

/// A declaration as code: its template head, the attributes written before
/// it, and the rest, which a reader takes in apart.
struct Definition {
    /// `template <...>`, when the attributes come between it and the rest.
    std::string head;

    /// The attribute specifiers written before the declaration, on a line of
    /// their own; empty for none.
    std::string attributes;

    std::string text;

    std::vector<AttributeMacro> attribute_macros;
};

/// Pretty-print the declaration itself, its attributes as written. A class
/// or enum with a visible definition prints that definition, its body
/// summarized as options.max_members asks. Initializers longer than 200
/// tokens, counted by `token_count` over a range, are suppressed — such lists
/// are not useful in a hover card and are catastrophically expensive to
/// print.
auto definition(const clang::Decl* decl,
                const Options& options,
                llvm::function_ref<std::size_t(clang::SourceRange)> token_count) -> Definition;

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Type& type);

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const Param& param);

}  // namespace clice::display
