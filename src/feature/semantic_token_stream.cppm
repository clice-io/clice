module;

#include "modules/prelude.h"

module clice:feature.semantic_token_stream;

import :compile.compilation;
import :feature.feature;
import :semantic.token_class;

namespace clice::feature {

/// How the placeholders of a format string are written.
enum class FormatStyle : std::uint8_t {
    /// `std::format`: `{}`, `{0:>8}`; `{{` and `}}` write braces.
    Std,
    /// printf: `%d`, `%-8.3f`; `%%` writes a percent sign.
    Printf,
};

/// What the AST or the index knows of a file's written tokens beyond their
/// spelling.
struct TokenFacts {
    /// The classification of the names, overloaded operators, declarator
    /// operators and user-defined literals the file writes, by the offset
    /// of the token each begins at. A name of several tokens (`~Widget`)
    /// gives each of them.
    llvm::DenseMap<std::uint32_t, TokenClass> semantic;

    /// The string literal tokens of format strings, by offset.
    llvm::DenseMap<std::uint32_t, FormatStyle> formats;

    /// Sorted, disjoint begin/end pairs of the text whose tokens are
    /// Inactive: preprocessor-inactive regions and the arguments a macro
    /// drops.
    std::vector<std::uint32_t> inactive;
};

/// Whether a name classified `token` paints a token of `kind`. A written
/// name is spelled as an identifier, a destructor's `~`, or for an
/// operator the punctuation or the user-defined literal spelling it; a name
/// resolving to any other token — an anonymous parameter's trailing `)`,
/// the `[` of a structured binding, the `(` of a constructor call written
/// as the type name, an operator declaration's `operator` keyword — names
/// nothing written.
bool names_token(clang::tok::TokenKind kind, const TokenClass& token);

/// Whether `spelling` is a contextual keyword of overriding: `override`,
/// `final`, MSVC's `sealed`.
bool is_override_keyword(llvm::StringRef spelling);

/// What the compiled unit knows of its focus file's tokens, inactive
/// regions merged in. With `index_known`, only what the file's index
/// records too, the facts its index route has: no inactive code or
/// dropped macro arguments, declarator operators, dependent or unresolved
/// names, or format strings.
auto semantic_token_facts(CompilationUnit& unit,
                          llvm::ArrayRef<std::uint32_t> inactive_regions,
                          bool index_known) -> TokenFacts;

/// The semantic tokens of `content`, the single writer of both the AST's
/// and the index's: every token lexed under `lang_opts` and classified by
/// `facts` where they say, by its spelling otherwise — keywords, literals
/// split into their prefix, escapes, placeholders and suffix, comments
/// with their documentation commands, directives, template brackets.
/// Adjacent tokens of one classification merge.
auto semantic_token_stream(llvm::StringRef content,
                           const clang::LangOptions& lang_opts,
                           const TokenFacts& facts,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken>;

}  // namespace clice::feature
