#pragma once

#include <cstdint>

#include "clang/AST/Decl.h"
#include "clang/Basic/TokenKinds.h"

namespace clice {

/// In the LSP, there are several different kinds, such as `SemanticTokenType`,
/// `CompletionItemKind`, and `SymbolKind`. Unfortunately, these kinds do not cover all the semantic
/// information we need. It's also inconsistent that some kinds exist in one category but not in
/// another, for example, `Namespace` is present in `SemanticTokenType` but not in
/// `CompletionItemKind`. To address this, we define our own `SymbolKind`, which will be used
/// consistently across our responses to the client and in the index. Users who prefer to stick to
/// standard LSP kinds can map our `SymbolKind` to the corresponding LSP kinds through
/// configuration.
struct SymbolKind {
    enum Kind : std::uint8_t {
        Comment = 0,     ///< C/C++ comments.
        Number,          ///< C/C++ number literal.
        Character,       ///< C/C++ character literal.
        String,          ///< C/C++ string literal.
        Keyword,         ///< C/C++ keyword.
        Directive,       ///< C/C++ preprocessor directive, e.g. `#include`.
        Header,          ///< C/C++ header name, e.g. `<iostream>` and `"foo.h"`.
        Module,          ///< C++20 module name.
        Macro,           ///< C/C++ macro.
        MacroParameter,  ///< C/C++ macro parameter.
        Namespace,       ///> C++ namespace.
        Class,           ///> C/C++ class.
        Struct,          ///> C/C++ struct.
        Union,           ///> C/C++ union.
        Enum,            ///> C/C++ enum.
        Type,            ///> C/C++ type alias and C++ template type parameter.
        Field,           ///> C/C++ field.
        EnumMember,      ///> C/C++ enum member.
        Function,        ///> C/C++ function.
        Method,          ///> C++ method.
        Variable,        ///> C/C++ variable, includes C++17 structured bindings.
        Parameter,       ///> C/C++ parameter.
        Label,           ///> C/C++ label.
        Concept,         ///> C++20 concept.
        Attribute,       ///> GNU/MSVC/C++11/C23 attribute.
        Operator,        ///> C/C++ operator.
        Paren,           ///> `(` and `)`.
        Bracket,         ///> `[` and `]`.
        Brace,           ///> `{` and `}`.
        Angle,           ///> `<` and `>`.
        Conflict,        ///> This token have multiple kinds.
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

    static SymbolKind from(const clang::tok::TokenKind kind);

private:
    Kind kind_value = Invalid;
};

struct SymbolModifiers {
    enum Kind : std::uint32_t {
        /// Represents that the symbol is a declaration(e.g. function declaration).
        Declaration = 0,

        /// Represents that the symbol is a definition(e.g. function definition).
        Definition = 1,

        /// Represents that the symbol is const modified(e.g. `const` variable).
        Const = 2,

        /// Represents that the symbol is overloaded(e.g. overloaded functions and operators).
        Overloaded = 3,

        /// Represents that the symbol is a part of type(e.g. `*` in `int*`).
        Typed = 4,

        /// Represents that the symbol is a template(e.g. class template or function template).
        Templated = 5,

        /// Represents that the symbol is deprecated.
        Deprecated = 6,

        /// Represents that the symbol is deduced.
        Deduced = 7,

        /// Represents that the symbol is readonly.
        Readonly = 8,

        /// Represents that the symbol is static.
        Static = 9,

        /// Represents that the symbol is abstract.
        Abstract = 10,

        /// Represents that the symbol is virtual.
        Virtual = 11,

        /// Represents that the symbol is a dependent name.
        DependentName = 12,

        /// Represents that the symbol comes from the default library.
        DefaultLibrary = 13,

        /// Represents that the symbol is used through a mutable reference.
        UsedAsMutableReference = 14,

        /// Represents that the symbol is used through a mutable pointer.
        UsedAsMutablePointer = 15,

        /// Represents that the symbol is a constructor or destructor.
        ConstructorOrDestructor = 16,

        /// Represents that the symbol is user-defined.
        UserDefined = 17,

        /// Represents that the symbol is function-scoped.
        FunctionScope = 18,

        /// Represents that the symbol is class-scoped.
        ClassScope = 19,

        /// Represents that the symbol is file-scoped.
        FileScope = 20,

        /// Represents that the symbol is global-scoped.
        GlobalScope = 21,
    };

    constexpr static std::uint32_t to_mask(Kind kind) {
        return std::uint32_t(1) << static_cast<std::uint32_t>(kind);
    }

    constexpr SymbolModifiers() = default;

    constexpr SymbolModifiers(Kind kind) : value(to_mask(kind)) {}

    constexpr explicit SymbolModifiers(std::uint32_t bits) : value(bits) {}

    constexpr operator std::uint32_t() const {
        return value;
    }

    constexpr bool contains(Kind kind) const {
        return (value & to_mask(kind)) != 0;
    }

private:
    std::uint32_t value = 0;
};

}  // namespace clice
