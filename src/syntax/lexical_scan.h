#pragma once

#include <cstdint>
#include <vector>

#include "syntax/token.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "clang/Basic/LangOptions.h"

namespace clice {

/// Everything one lexical pass over a file records that neither the AST
/// nor the preprocessor callbacks report: comments (the token buffer drops
/// them), the three module declaration forms (clang reports imports
/// through PPCallbacks, but nothing covers the declarations themselves),
/// the block structure of the conditional and region directives (the
/// callbacks skip the branches nested in a skipped block, a `#else` behind
/// a taken `#elif`, and everything a preamble PCH consumed), the include
/// directives (a preamble PCH consumes those too) and the extents of raw
/// string literals.
struct LexicalInfo {
    struct Comment {
        enum class Kind : std::uint8_t {
            /// A `//` comment.
            Line,
            /// A `/* */` comment.
            Block,
        };

        Kind kind;
        LocalSourceRange range;
    };

    struct ModuleDeclaration {
        enum class Kind : std::uint8_t {
            /// `module;` introducing the global module fragment.
            GlobalFragment,
            /// `[export] module name[:partition];`
            Declaration,
            /// `module :private;` introducing the private module fragment.
            PrivateFragment,
        };

        Kind kind;

        /// The `export` keyword; valid only when a Declaration has one.
        LocalSourceRange export_keyword;

        /// The `module` keyword.
        LocalSourceRange keyword;

        /// The identifiers of the dotted module name, in source order.
        /// Empty for the fragment introducers.
        llvm::SmallVector<LocalSourceRange, 4> name_parts;

        /// The partition colon; invalid when there is no partition. Also
        /// set for the private fragment (`:private`).
        LocalSourceRange colon;

        /// The identifiers of the dotted partition name; the `private`
        /// keyword for the private fragment.
        llvm::SmallVector<LocalSourceRange, 2> partition_parts;
    };

    /// A directive opening, continuing or closing a block of lines,
    /// whether or not the preprocessor entered it.
    struct BlockDirective {
        enum class Kind : std::uint8_t {
            /// `#if`, `#ifdef` or `#ifndef`.
            If,
            /// `#elif`, `#elifdef`, `#elifndef` or `#else`.
            Else,
            /// `#endif`.
            EndIf,
            /// `#pragma region`.
            Region,
            /// `#pragma endregion`.
            EndRegion,
        };

        Kind kind;

        /// From the `#` to the end of the directive's logical line.
        LocalSourceRange range;
    };

    /// In source order.
    std::vector<BlockDirective> block_directives;

    /// `#include`, `#include_next` and `#import` directives in source
    /// order, each from the `#` to the end of its logical line.
    std::vector<LocalSourceRange> include_directives;

    /// Raw string literal tokens in source order, outside directives.
    std::vector<LocalSourceRange> raw_strings;

    // Both vectors heap-allocate so that payload pointers into them (the
    // Semantics node table stores such pointers) survive moving the info.
    std::vector<Comment> comments;

    std::vector<ModuleDeclaration> modules;
};

/// Scan `content` once and collect its LexicalInfo. The matching is purely
/// lexical: module declarations are recognized by line-start grammar, so
/// consumers integrating them must cross-check compiler state (e.g. the
/// unit's named module) before trusting them.
LexicalInfo lexical_scan(llvm::StringRef content, const clang::LangOptions* lang_opts = nullptr);

}  // namespace clice
