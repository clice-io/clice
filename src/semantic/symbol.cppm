module clice:semantic.symbol;

import :support.prelude;

namespace clice {

/// What a symbol is, in one vocabulary across the index, the outline,
/// hover and completion; each LSP reply maps it to the kind set that reply
/// takes. Semantic tokens have their own (SemanticTokenKind).
struct SymbolKind {
    enum Kind : std::uint8_t {
        Comment = 0,  ///< C/C++ comments.
        Number,       ///< C/C++ number literal.
        Character,    ///< C/C++ character literal.
        String,       ///< C/C++ string literal.
        Keyword,      ///< C/C++ keyword.
        Directive,    ///< C/C++ preprocessor directive, e.g. `#include`.
        Header,       ///< C/C++ header name, e.g. `<iostream>` and `"foo.h"`.
        Module,       ///< C++20 module name.
        Macro,        ///< C/C++ macro.
        Namespace,    ///> C++ namespace.
        Class,        ///> C/C++ class.
        Struct,       ///> C/C++ struct.
        Union,        ///> C/C++ union.
        Enum,         ///> C/C++ enum.
        Type,         ///> C/C++ type alias and C++ template type parameter.
        Field,        ///> C/C++ field.
        EnumMember,   ///> C/C++ enum member.
        Function,     ///> C/C++ function.
        Method,       ///> C++ method.
        Variable,     ///> C/C++ variable, includes C++17 structured bindings.
        Parameter,    ///> C/C++ parameter.
        Label,        ///> C/C++ label.
        Concept,      ///> C++20 concept.
        Attribute,    ///> GNU/MSVC/C++11/C23 attribute.
        Operator,     ///> C/C++ operator.
        Invalid,
    };

    constexpr SymbolKind() = default;

    constexpr SymbolKind(Kind kind) : kind_value(kind) {}

    constexpr explicit SymbolKind(std::uint8_t kind) : kind_value(static_cast<Kind>(kind)) {}

    constexpr operator Kind() const {
        return kind_value;
    }

    constexpr std::uint8_t value_of() const {
        return static_cast<std::uint8_t>(kind_value);
    }

    constexpr std::uint8_t value() const {
        return value_of();
    }

    static SymbolKind from(const clang::Decl* decl);

private:
    Kind kind_value = Invalid;
};

struct RelationKind {
    enum Kind : std::uint32_t {
        Invalid = 0,
        Declaration,
        Definition,
        Reference,
        WeakReference,
        Read,
        /// A reference that modifies what it names (is_written), recorded
        /// next to the Reference relation of the same name.
        Write,
        Interface,
        Implementation,
        TypeDefinition,
        Base,
        Derived,
        Constructor,
        Destructor,
        Caller,
        Callee,
        /// Keyed by a template (or a member of a class template), the
        /// target an explicit or partial specialization of it.
        Specialization,
        /// Keyed by an explicit or partial specialization, the target the
        /// template it specializes.
        Primary,
        /// At the include that pastes a fragment into a declaration
        /// (`#include "Kinds.inc"` inside a switch), a name the fragment
        /// uses: the pasting file names it too.
        Pasted,
        /// At a keyword standing for a type it does not spell — `auto`,
        /// `decltype(auto)`, `decltype(expr)` — the declaration that type
        /// names: navigation from the keyword reaches it, but the keyword
        /// is no reference to it.
        Deduced,
        /// At a keyword that leads to a declaration it does not spell —
        /// `override` and `final` to the methods they override, `new` to
        /// the constructor it runs, which its parentheses reference — the
        /// declaration: navigation from the keyword reaches it, but the
        /// keyword is no reference to it.
        Implied,
        /// At an explicit instantiation's name (`template struct Box<int>;`),
        /// the specialization it instantiates, with the directive's extent:
        /// the outline lists the directive, which navigation reaches through
        /// the Reference beside it and never as a declaration.
        Instantiation,
        /// A virtual function named by qualification (`Base::f()`),
        /// recorded next to the Reference of the same name: the call runs
        /// that function, never an override.
        Direct,
    };

    constexpr RelationKind() = default;

    constexpr RelationKind(Kind kind) : kind_value(kind) {}

    constexpr operator Kind() const {
        return kind_value;
    }

    constexpr std::uint32_t value() const {
        return static_cast<std::uint32_t>(kind_value);
    }

    template <typename... Kinds>
    constexpr bool is_one_of(Kinds... kinds) const {
        return ((kind_value == kinds) || ...);
    }

    constexpr bool isDeclOrDef() const {
        return is_one_of(Declaration, Definition);
    }

    /// Whether the relation's target field holds the extent of what it
    /// declares rather than a symbol.
    constexpr bool carries_extent() const {
        return is_one_of(Declaration, Definition, Instantiation);
    }

    constexpr bool isReference() const {
        return is_one_of(Reference, WeakReference);
    }

    constexpr bool isBetweenSymbol() const {
        return is_one_of(Interface,
                         Implementation,
                         TypeDefinition,
                         Base,
                         Derived,
                         Constructor,
                         Destructor,
                         Specialization,
                         Primary);
    }

private:
    Kind kind_value = Invalid;
};

constexpr bool operator==(RelationKind lhs, RelationKind rhs) {
    return lhs.value() == rhs.value();
}

}  // namespace clice

/// SymbolKind hides its enum behind constructors, which keeps it out of
/// reflection; it serializes as that enum.
template <>
struct kotatsu::meta::repr<clice::SymbolKind> {
    using type = clice::SymbolKind::Kind;

    static type to(clice::SymbolKind kind) {
        return kind;
    }

    static clice::SymbolKind from(type kind) {
        return kind;
    }
};
