#pragma once

#include "Support/Enum.h"

#include "clang/AST/Decl.h"

namespace clice {

/// In the LSP, there are several different kinds, such as `SemanticTokenType`,
/// `CompletionItemKind`, and `SymbolKind`. Unfortunately, these kinds do not cover all the semantic
/// information we need. It's also inconsistent that some kinds exist in one category but not in
/// another, for example, `Namespace` is present in `SemanticTokenType` but not in
/// `CompletionItemKind`. To address this, we define our own `SymbolKind`, which will be used
/// consistently across our responses to the client and in the index. Users who prefer to stick to
/// standard LSP kinds can map our `SymbolKind` to the corresponding LSP kinds through
/// configuration.
struct SymbolKind : refl::Enum<SymbolKind, false, uint8_t> {
    enum Kind : uint8_t {
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

    using Enum::Enum;

    constexpr inline static auto InvalidEnum = Kind::Invalid;

    static SymbolKind from(const clang::Decl* decl);

    static SymbolKind from(const clang::tok::TokenKind kind);
};

struct SymbolModifiers : refl::Enum<SymbolModifiers, true, uint32_t> {
    enum Kind {
        /// Represents that the symbol is a declaration(e.g. function declaration).
        Declaration = 0,

        /// Represents that the symbol is a definition(e.g. function definition).
        Definition,

        /// Represents that the symbol is const modified(e.g. `const` variable).
        Const,

        /// Represents that the symbol is overloaded(e.g. overloaded functions and operators).
        Overloaded,

        /// Represents that the symbol is a part of type(e.g. `*` in `int*`).
        Typed,

        /// Represents that the symbol is a template(e.g. class template or function template).
        Templated,
    };

    using Enum::Enum;
};

}  // namespace clice
