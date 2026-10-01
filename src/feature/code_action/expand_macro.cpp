#include <format>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "syntax/lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"

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

/// Whether `offset` lies in a preprocessor directive, whose macro
/// references (`#if GUARD`) expand to nothing in the token stream; a
/// directive goes on past every line ending in a backslash.
bool in_directive(llvm::StringRef content, std::uint32_t offset) {
    auto begin = line_begin(content, offset);
    while(begin > 0 && content.substr(0, begin - 1).rtrim(" \t\r").ends_with('\\')) {
        begin = line_begin(content, begin - 1);
    }
    return content.substr(begin).ltrim(" \t").starts_with('#');
}

/// Whether `left` written right before `right` lexes as other tokens:
/// `-` `-` as a decrement, `/` `*` as a comment, `:` `::` as `::` `:`.
bool glues(llvm::StringRef left, llvm::StringRef right, const clang::LangOptions& options) {
    auto text = (left + right).str();
    Lexer lexer(text, {.keep_comments = true, .lang_opts = &options});
    return lexer.advance().range.end != left.size();
}

}  // namespace

void expand_macro(CompilationUnitRef unit,
                  LocalSourceRange selection,
                  std::vector<CodeAction>& out) {
    auto main = unit.main_file();
    auto touching = unit.spelled_tokens_touch(unit.create_location(main, selection.begin));
    if(touching.empty()) {
        return;
    }
    auto& SM = unit.context().getSourceManager();
    const auto& options = unit.lang_options();
    auto content = unit.main_content();
    for(const auto& expansion: unit.expansions_overlapping(touching)) {
        // Directives are mappings too, from their `#`.
        if(expansion.Spelled.empty() ||
           expansion.Spelled.front().kind() != clang::tok::identifier) {
            continue;
        }
        const auto& name = expansion.Spelled.front();
        auto begin = unit.file_offset(name.location());
        auto end = unit.file_offset(expansion.Spelled.back().endLocation());
        if(in_directive(content, begin)) {
            continue;
        }
        // A pragma the expansion executes leaves no token behind.
        if(llvm::any_of(unit.directives()[main].pragma_operators, [&](clang::SourceLocation loc) {
               auto offset = unit.file_offset(unit.expansion_location(loc));
               return begin <= offset && offset < end;
           })) {
            return;
        }

        auto expanded = expansion.Expanded;
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
        auto spelled = unit.spelled_tokens(main);
        auto first = static_cast<std::size_t>(expansion.Spelled.begin() - spelled.begin());
        auto last = first + expansion.Spelled.size();
        std::optional<llvm::StringRef> preceding;
        if(first > 0 && unit.file_offset(spelled[first - 1].endLocation()) == begin) {
            preceding = spelled[first - 1].text(SM);
        }
        std::optional<llvm::StringRef> following;
        if(last < spelled.size() && unit.file_offset(spelled[last].location()) == end) {
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
            .title = std::format("Expand macro '{}'", name.text(SM)),
            .kind = protocol::CodeActionKind::refactor_inline,
            .edits = {{{begin, end}, std::move(text)}},
        });
        return;
    }
}

}  // namespace clice::feature::action
