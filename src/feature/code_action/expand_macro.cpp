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

/// The length of the line splice `text` starts with: a backslash, the
/// blanks clang tolerates after it, and a line break; zero for none.
std::size_t splice_length(llvm::StringRef text) {
    if(!text.starts_with('\\')) {
        return 0;
    }
    auto rest = text.drop_front().ltrim(" \t\v\f");
    auto skipped = text.size() - rest.size();
    if(rest.starts_with("\r\n")) {
        return skipped + 2;
    }
    return rest.starts_with('\n') || rest.starts_with('\r') ? skipped + 1 : 0;
}

/// Whether the text between two tokens ends a line: it breaks one outside
/// a block comment and a splice, or holds a line comment, which runs to
/// such a break.
bool ends_line(llvm::StringRef gap) {
    while(!gap.empty()) {
        if(gap.starts_with("//")) {
            return true;
        }
        if(gap.starts_with("/*")) {
            auto close = gap.find("*/", 2);
            gap = close == llvm::StringRef::npos ? "" : gap.drop_front(close + 2);
        } else if(auto splice = splice_length(gap)) {
            gap = gap.drop_front(splice);
        } else if(gap.front() == '\n' || gap.front() == '\r') {
            return true;
        } else {
            gap = gap.drop_front();
        }
    }
    return false;
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
    auto spelled = unit.spelled_tokens(main);
    for(const auto& expansion: unit.expansions_overlapping(touching)) {
        // Directives are mappings too, from their `#`. A name the raw lexer
        // must clean, one starting with a line splice, stays raw.
        if(expansion.Spelled.empty() ||
           !llvm::is_contained({clang::tok::identifier, clang::tok::raw_identifier},
                               expansion.Spelled.front().kind())) {
            continue;
        }
        const auto& name = expansion.Spelled.front();
        auto begin = unit.file_offset(name.location());
        auto end = unit.file_offset(expansion.Spelled.back().endLocation());
        auto first = static_cast<std::size_t>(expansion.Spelled.begin() - spelled.begin());
        auto last = first + expansion.Spelled.size();
        auto gap = [&](std::size_t index) {
            return content.slice(unit.file_offset(spelled[index - 1].endLocation()),
                                 unit.file_offset(spelled[index].location()));
        };
        // Macro references in a directive (`#if GUARD`) expand to nothing
        // in the token stream. The directive is the line starting with
        // `#`, which goes on past splices and block comments.
        auto line = first;
        while(line > 0 && !ends_line(gap(line))) {
            --line;
        }
        if(spelled[line].kind() == clang::tok::hash) {
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
            .kind = protocol::CodeActionKind::RefactorInline,
            .edits = {{{begin, end}, std::move(text)}},
        });
        return;
    }
}

}  // namespace clice::feature::action
