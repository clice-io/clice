module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.code_action.action;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice::feature::action {

namespace {

/// Whether two adjacent expanded tokens are written with a space between
/// them: none inside brackets, around member and scope operators, between
/// a name and the parenthesis calling it, or after a prefix operator — one
/// not following an operand, `before` being the token ahead of `left`
/// (`unknown` at the start); one everywhere else.
bool needs_space(clang::tok::TokenKind before,
                 clang::tok::TokenKind left,
                 clang::tok::TokenKind right) {
    using namespace clang::tok;
    if(left == identifier && right == l_paren) {
        return false;
    }
    switch(right) {
        case r_paren:
        case r_square:
        case comma:
        case semi:
        case period:
        case arrow:
        case coloncolon: return false;
        default: break;
    }
    switch(left) {
        case l_paren:
        case l_square:
        case period:
        case arrow:
        case coloncolon: return false;
        case star:
        case amp:
        case minus:
        case plus:
        case exclaim:
        case tilde:
            return before == identifier || before == r_paren || before == r_square ||
                   isLiteral(before);
        default: return true;
    }
}

/// Whether `left` written right before `right` lexes as other tokens:
/// `-` `-` as a decrement, `/` `*` as a comment, `:` `::` as `::` `:`.
bool glues(llvm::StringRef left, llvm::StringRef right, const clang::LangOptions& options) {
    auto text = (left + right).str();
    Lexer lexer(text, options, {.keep_comments = true});
    return lexer.advance().range.end != left.size();
}

}  // namespace

void expand_macro(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out) {
    auto focus = unit.focus_file();
    auto& tokens = focus.tokens();
    auto touching = tokens.spelled_touching(focus.location(selection.begin));
    if(touching.empty()) {
        return;
    }
    auto& SM = unit.context().getSourceManager();
    const auto& options = unit.lang_options();
    auto content = focus.content();
    auto spelled = tokens.spelled();
    auto end_of = [&](const clang::syntax::Token& token) {
        return tokens.offset(token) + token.length();
    };
    for(const auto& expansion: tokens.expansions_overlapping(touching)) {
        // A name the raw lexer must clean, one starting with a line splice,
        // stays raw.
        if(!llvm::is_contained({clang::tok::identifier, clang::tok::raw_identifier},
                               expansion.spelled.front().kind())) {
            continue;
        }
        const auto& name = expansion.spelled.front();
        auto begin = tokens.offset(name);
        auto end = end_of(expansion.spelled.back());
        auto first = static_cast<std::size_t>(expansion.spelled.begin() - spelled.begin());
        auto last = first + expansion.spelled.size();
        auto gap = [&](std::size_t index) {
            return content.slice(end_of(spelled[index - 1]), tokens.offset(spelled[index]));
        };
        // Macro references in a directive (`#if GUARD`) expand to nothing
        // in the token stream.
        if(focus.lexical().directive_at(content, begin)) {
            continue;
        }
        // A pragma the expansion executes leaves no token behind.
        if(llvm::any_of(unit.directives()[focus.file()].pragma_operators,
                        [&](clang::SourceLocation loc) {
                            auto offset = unit.produced_at(loc).offset;
                            return begin <= offset && offset < end;
                        })) {
            return;
        }

        auto expanded = expansion.expanded;
        std::string text;
        for(auto [index, token]: llvm::enumerate(expanded)) {
            if(index > 0) {
                const auto& left = expanded[index - 1];
                auto before = index > 1 ? expanded[index - 2].kind() : clang::tok::unknown;
                if(needs_space(before, left.kind(), token.kind()) ||
                   glues(left.text(SM), token.text(SM), options)) {
                    text += ' ';
                }
            }
            text += token.text(SM);
        }

        // The text lands between the tokens written flush against the
        // invocation, which must not run into it either.
        std::optional<llvm::StringRef> preceding;
        if(first > 0 && gap(first).empty()) {
            preceding = spelled[first - 1].text(SM);
        }
        std::optional<llvm::StringRef> following;
        if(last < spelled.size() && gap(last).empty()) {
            following = spelled[last].text(SM);
        }
        auto head = expanded.empty() ? following : expanded.front().text(SM);
        if(preceding && head && glues(*preceding, *head, options)) {
            text.insert(text.begin(), ' ');
        }
        if(following && !expanded.empty() && glues(expanded.back().text(SM), *following, options)) {
            text += ' ';
        }

        out.push_back(CodeAction{
            .title = std::format("Expand macro '{}'", unit.token_spelling(name.location())),
            .kind = action_kind::expand_macro,
            .edits = {{{begin, end}, std::move(text)}},
        });
        return;
    }
}

}  // namespace clice::feature::action
