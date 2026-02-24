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
        Declaration = 1u << 0,

        /// Represents that the symbol is a definition(e.g. function definition).
        Definition = 1u << 1,

        /// Represents that the symbol is const modified(e.g. `const` variable).
        Const = 1u << 2,

        /// Represents that the symbol is overloaded(e.g. overloaded functions and operators).
        Overloaded = 1u << 3,

        /// Represents that the symbol is a part of type(e.g. `*` in `int*`).
        Typed = 1u << 4,

        /// Represents that the symbol is a template(e.g. class template or function template).
        Templated = 1u << 5,
    };

    constexpr SymbolModifiers() = default;

    constexpr SymbolModifiers(Kind kind) : value(static_cast<std::uint32_t>(kind)) {}

    constexpr explicit SymbolModifiers(std::uint32_t bits) : value(bits) {}

    constexpr operator std::uint32_t() const {
        return value;
    }

    constexpr bool contains(Kind kind) const {
        return (value & static_cast<std::uint32_t>(kind)) != 0;
    }

private:
    std::uint32_t value = 0;
};

}  // namespace clice
