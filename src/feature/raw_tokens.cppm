module;

#include "modules/prelude.h"

module clice:feature.raw_tokens;

import :compile.compilation;
import :syntax.token;

namespace clice::feature {

/// The main file's tokens as a raw lex sees them, comments, directives and
/// the stretches the preprocessor skipped included.
std::vector<Token> lex(llvm::StringRef content, const clang::LangOptions& lang_opts);

/// Per raw token, whether the preprocessor skipped it; a token the unit's
/// spelled tokens do not start at (a comment, a directive's end) shares
/// the flag of the one before.
llvm::SmallVector<bool, 0> skipped_tokens(CompilationUnit& unit, llvm::ArrayRef<Token> tokens);

/// Bracket pairs over raw tokens fed in order. Brackets pair within their
/// scope — a directive line, a stretch the preprocessor skipped — and
/// outside any across the file; a closer without an opener of its kind (a
/// conditional's branches can leave both unbalanced) closes nothing, and
/// the openers a scope leaves open close with it.
class BracketPairs {
public:
    /// The opener `token` closes, if it is a closer that pairs. `skipped`
    /// tells whether the preprocessor skipped it.
    std::optional<LocalSourceRange> visit(const Token& token, bool skipped);

    /// Whether an opener is still open.
    bool inside() const {
        return !opens.empty();
    }

    bool in_directive() const {
        return directive;
    }

    bool in_skipped() const {
        return skipping;
    }

private:
    void enter();

    void leave();

    struct Open {
        TokenKind closer;
        LocalSourceRange range;
    };

    llvm::SmallVector<Open> opens;
    llvm::SmallVector<std::size_t, 2> scopes;
    bool directive = false;
    bool skipping = false;
};

}  // namespace clice::feature
