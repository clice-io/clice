module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.raw_tokens;
import :semantic.semantics;
import :syntax.lexer;

namespace clice::feature {

std::vector<Token> lex(llvm::StringRef content, const clang::LangOptions& lang_opts) {
    std::vector<Token> tokens;
    Lexer lexer(content, {.keep_comments = true, .lang_opts = &lang_opts});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        tokens.push_back(token);
    }
    return tokens;
}

llvm::SmallVector<bool, 0> skipped_tokens(CompilationUnit& unit, llvm::ArrayRef<Token> tokens) {
    const auto& semantics = unit.get<Semantics>();
    auto count = static_cast<std::uint32_t>(semantics.spelled_tokens().size());
    llvm::SmallVector<bool, 0> skipped(tokens.size());
    std::uint32_t spelled = 0;
    bool current = false;
    for(std::size_t i = 0; i < tokens.size(); i += 1) {
        auto begin = tokens[i].range.begin;
        while(spelled < count && semantics.token_offset(spelled) < begin) {
            spelled += 1;
        }
        if(spelled < count && semantics.token_offset(spelled) == begin) {
            current = semantics.token_preprocessed_away(spelled);
        }
        skipped[i] = current;
    }
    return skipped;
}

static TokenKind closer(TokenKind opener) {
    switch(opener) {
        case clang::tok::l_paren: return clang::tok::r_paren;
        case clang::tok::l_square: return clang::tok::r_square;
        case clang::tok::l_brace: return clang::tok::r_brace;
        default: return clang::tok::unknown;
    }
}

void BracketPairs::enter() {
    scopes.push_back(opens.size());
}

void BracketPairs::leave() {
    opens.truncate(scopes.pop_back_val());
}

std::optional<LocalSourceRange> BracketPairs::visit(const Token& token, bool skipped) {
    if(token.kind == clang::tok::comment) {
        return std::nullopt;
    }
    if(token.is_eod()) {
        if(directive) {
            directive = false;
            leave();
        }
        return std::nullopt;
    }
    if(skipped != skipping && !directive) {
        skipping = skipped;
        if(skipping) {
            enter();
        } else {
            leave();
        }
    }
    if(token.is_directive_hash()) {
        directive = true;
        enter();
        return std::nullopt;
    }

    if(auto kind = closer(token.kind); kind != clang::tok::unknown) {
        opens.push_back({kind, token.range});
        return std::nullopt;
    }
    if(!llvm::is_contained({clang::tok::r_paren, clang::tok::r_square, clang::tok::r_brace},
                           token.kind)) {
        return std::nullopt;
    }
    auto bottom = scopes.empty() ? 0 : scopes.back();
    for(auto i = opens.size(); i > bottom; i -= 1) {
        if(opens[i - 1].closer == token.kind) {
            auto range = opens[i - 1].range;
            opens.truncate(i - 1);
            return range;
        }
    }
    return std::nullopt;
}

}  // namespace clice::feature
