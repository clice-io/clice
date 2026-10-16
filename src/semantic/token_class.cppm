module;

#include "modules/prelude.h"

module clice:semantic.token_class;

namespace clice {

/// What a semantic token is, in clice's own words: the legend names them in
/// this order, lower camel case, unless the `[semantic_tokens]` config maps
/// them to the names a client expects. The index stores a symbol's kind:
/// new kinds go last.
enum class SemanticTokenKind : std::uint8_t {
    /// No token: a declaration that names nothing written (an anonymous
    /// entity, a using-directive).
    None = 0,
    Comment,
    String,
    Character,
    Number,
    /// An escape sequence inside a string or character literal.
    Escape,
    /// A replacement field of a `std::format` string (`{}`, `{:>8}`) or a
    /// conversion of a printf one (`%d`).
    FormatSpecifier,
    Keyword,
    /// A built-in type: `int`, `bool`, `__int128`.
    Primitive,
    Operator,
    /// The `<` and `>` of a template argument or parameter list.
    Bracket,
    /// A preprocessor directive's `#` and name.
    Directive,
    /// The header an `#include`, `__has_include` or `import` names.
    Header,
    Namespace,
    Module,
    Class,
    Struct,
    Union,
    Enum,
    /// A typedef or using alias, and a dependent type named with
    /// `typename T::type`.
    TypeAlias,
    /// A template's type or template template parameter.
    TypeParameter,
    /// A template's non-type parameter.
    TemplateParameter,
    Concept,
    Function,
    Method,
    Variable,
    Parameter,
    Field,
    EnumMember,
    Label,
    Macro,
    MacroParameter,
    /// The name of an attribute: `nodiscard`, `gnu::always_inline`.
    Attribute,
    /// A name only an instantiation resolves: a member of a bare template
    /// parameter (`T::reset()`, `value.size()`).
    Unknown,
    /// A name meaning entities of different kinds at once (a type and a
    /// function of one name, instantiations that disagree).
    Conflict,
    /// A token with no kind of its own in a preprocessor-inactive region,
    /// carrying its Inactive modifier; with lexical tokens off, every token
    /// there.
    Plain,
};

/// Whether every byte of an index column names a kind: one past the enum
/// would index the legend's tables out of bounds.
inline bool all_token_kinds(llvm::ArrayRef<std::uint8_t> kinds) {
    return llvm::all_of(kinds, [](std::uint8_t kind) {
        return kind <= static_cast<std::uint8_t>(SemanticTokenKind::Plain);
    });
}

/// The bits of a semantic token's modifier set, in bit order. A modifier
/// named like a standard LSP one means what that one means on every kind;
/// the others may mean something of their own on each kind, documented on
/// the enumerator. The index stores the ones a symbol carries everywhere:
/// new modifiers go last.
enum class SemanticTokenModifier : std::uint8_t {
    Declaration,
    Definition,
    /// The named object cannot be modified: a const object or a reference
    /// to one, a const member function, an enumerator, a non-type template
    /// parameter. A pointer to const is not readonly.
    Readonly,
    /// A static member, a static local variable.
    Static,
    Deprecated,
    /// An abstract class, a pure virtual function.
    Abstract,
    /// Declared in a system header, or built into the compiler.
    DefaultLibrary,
    /// Inside a documentation comment: a command (`@param`), the name a
    /// command refers to.
    Documentation,
    /// The occurrence writes what it names: the target of an assignment,
    /// the operand of `++`/`--`, an argument bound to a mutable reference.
    Modification,
    /// Visible inside a function: a local variable, a parameter, a label.
    Local,
    /// Visible inside a class: a member, a nested type.
    Member,
    /// Visible in its own translation unit only: internal linkage.
    Internal,
    /// Visible from other translation units.
    Global,
    /// A class, function, variable or alias template, or a specialization
    /// of one: what template arguments may follow.
    Template,
    /// A name depending on a template parameter, resolved through the
    /// template it names or not at all.
    Dependent,
    /// A virtual function; a class with virtual functions.
    Virtual,
    /// A constexpr or consteval function; a constexpr or constinit
    /// variable; a non-type template parameter.
    Constexpr,
    Constructor,
    Destructor,
    /// A function-like macro; a variable, parameter or field of a callable
    /// type: a function pointer or reference, a lambda, a class with an
    /// `operator()`.
    Callable,
    /// A keyword transferring control (`if`, `return`, `throw`, `co_await`);
    /// a function that never returns (`[[noreturn]]`); a macro whose
    /// expansion returns, breaks, continues, jumps or throws.
    ControlFlow,
    /// An operator calling an overloaded operator function; the suffix of
    /// a user-defined literal.
    UserDefined,
    /// The `*`, `&` or `&&` of a declarator: part of a type, not an
    /// operation.
    Declarator,
    /// `true`, `false`, `nullptr`.
    Literal,
    /// The encoding prefix or the suffix of a literal (`L`, `u8`, `42u`,
    /// `3.14f`).
    Affix,
    /// A macro expanding to nothing.
    Empty,
    /// In a preprocessor-inactive region, or an argument a macro drops.
    Inactive,
};

constexpr std::uint32_t mask(SemanticTokenModifier modifier) {
    return std::uint32_t(1) << static_cast<std::uint32_t>(modifier);
}

/// A token's kind and modifier set; for a symbol, what each occurrence of
/// it carries whatever the occurrence.
struct TokenClass {
    SemanticTokenKind kind = SemanticTokenKind::None;
    std::uint32_t modifiers = 0;

    bool has(SemanticTokenModifier modifier) const {
        return (modifiers & mask(modifier)) != 0;
    }

    friend bool operator==(const TokenClass&, const TokenClass&) = default;
};

/// Merge a candidate into a token's running classification: differing
/// kinds collapse to Conflict, and only the modifiers every candidate
/// agrees on survive — a token combining resolutions from several
/// instantiations must not depend on the order they were written in.
inline void combine(TokenClass& result, TokenClass candidate) {
    if(candidate.kind == SemanticTokenKind::None) {
        return;
    }
    if(result.kind == SemanticTokenKind::None) {
        result = candidate;
        return;
    }
    result.modifiers &= candidate.modifiers;
    if(result.kind != candidate.kind) {
        result.kind = SemanticTokenKind::Conflict;
    }
}

/// Whether a statement led by `kind` leaves the code around it: `return`,
/// `break`, `continue`, `goto`, `throw`, `co_return`.
inline bool leaves_enclosing(clang::tok::TokenKind kind) {
    return llvm::is_contained({clang::tok::kw_return,
                               clang::tok::kw_break,
                               clang::tok::kw_continue,
                               clang::tok::kw_goto,
                               clang::tok::kw_throw,
                               clang::tok::kw_co_return},
                              kind);
}

/// What every written name of `decl` shows: its kind and the modifiers it
/// carries at each occurrence. The tokens of the AST and the symbols the
/// index stores for its projection both come from here.
TokenClass token_class(const clang::NamedDecl* decl);

/// The same of the macro defined by `macro`.
TokenClass token_class(const clang::MacroInfo& macro, const clang::SourceManager& sources);

/// The classification of a keyword token: a built-in type, an operator
/// spelled as a word (`sizeof`, `new`, `and`), a keyword transferring
/// control or a literal one.
TokenClass keyword_class(clang::tok::TokenKind kind);

/// The keyword token `name` spells in some C or C++ dialect, identifier
/// when none.
clang::tok::TokenKind keyword_of(llvm::StringRef name);

}  // namespace clice
