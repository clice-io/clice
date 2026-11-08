module clice;

import :compile.compilation;
import :feature.code_actions.action;
import :feature.formatting;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice::feature::action {

namespace {

/// The expansion `joined`, tokens a space apart, spaced as clang-format
/// spaces it in place of the invocation at `invocation` in `content` — in
/// the style configured for `file`, else LLVM's — and laid on one line, as
/// the invocation was. Only the spaces between its own tokens change: the
/// statement around it tells whether a leading `-` or `&` is unary, and the
/// spaces around it keep it from running into its neighbours.
std::string spaced(llvm::StringRef file,
                   llvm::StringRef content,
                   LocalSourceRange invocation,
                   llvm::StringRef joined) {
    auto style = configured_style(file).value_or(clang::format::getLLVMStyle());
    style.ColumnLimit = 0;
    style.BreakStringLiterals = false;
    auto code = (content.take_front(invocation.begin) + " " + joined + " " +
                 content.drop_front(invocation.end))
                    .str();
    auto begin = invocation.begin + 1;
    auto end = begin + static_cast<std::uint32_t>(joined.size());
    clang::tooling::Replacements inner;
    for(const auto& replacement:
        clang::format::reformat(style, code, {clang::tooling::Range(begin, joined.size())}, file)) {
        auto offset = replacement.getOffset();
        if(offset > begin && offset + replacement.getLength() < end) {
            llvm::cantFail(
                inner.add(clang::tooling::Replacement(file,
                                                      offset - begin,
                                                      replacement.getLength(),
                                                      replacement.getReplacementText())));
        }
    }
    auto formatted = clang::tooling::applyAllReplacements(joined, inner);
    if(!formatted) {
        llvm::consumeError(formatted.takeError());
        return joined.str();
    }
    std::string line;
    llvm::SmallVector<llvm::StringRef> lines;
    llvm::StringRef(*formatted).split(lines, '\n', -1, /*KeepEmpty=*/false);
    for(auto each: lines) {
        if(each = each.trim(); !each.empty()) {
            if(!line.empty()) {
                line += ' ';
            }
            line += each;
        }
    }
    return line;
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
    for(const auto& expansion: tokens.expansions_overlapping(touching)) {
        // A macro named after a keyword (`#define inline`) stays as written.
        const auto& name = expansion.spelled.front();
        if(focus.kind(name) != clang::tok::identifier) {
            continue;
        }
        auto begin = name.range.begin;
        auto end = expansion.spelled.back().range.end;
        auto first = static_cast<std::size_t>(expansion.spelled.begin() - spelled.begin());
        auto last = first + expansion.spelled.size();
        auto gap = [&](std::size_t index) {
            return content.slice(spelled[index - 1].range.end, spelled[index].range.begin);
        };
        // Macro references in a directive (`#if GUARD`) expand to nothing
        // in the token stream.
        if(focus.lexical().directive_at(content, begin)) {
            continue;
        }
        // A pragma the expansion executes leaves no token behind.
        if(ranges::any_of(unit.directives()[focus.file()].pragma_operators,
                          [&](clang::SourceLocation loc) {
                              auto offset = unit.produced_at(loc).offset;
                              return begin <= offset && offset < end;
                          })) {
            return;
        }

        auto expanded = expansion.expanded;
        std::string joined;
        for(const auto& token: expanded) {
            if(!joined.empty()) {
                joined += ' ';
            }
            joined += token.text(SM);
        }
        auto text = spaced(unit.file_path(focus.file()), content, {begin, end}, joined);

        // The text lands between the tokens written flush against the
        // invocation, which must not run into it either; the end of a
        // directive on the line before is its line break, not one of them.
        std::optional<llvm::StringRef> preceding;
        if(first > 0 && gap(first).empty() && !spelled[first - 1].is_eod()) {
            preceding = spelled[first - 1].text(content);
        }
        std::optional<llvm::StringRef> following;
        if(last < spelled.size() && gap(last).empty()) {
            following = spelled[last].text(content);
        }
        auto head = expanded.empty() ? following : expanded.front().text(SM);
        if(preceding && head && glues(*preceding, *head, options)) {
            text.insert(text.begin(), ' ');
        }
        if(following && !expanded.empty() && glues(expanded.back().text(SM), *following, options)) {
            text += ' ';
        }

        out.push_back(CodeAction{
            .title = std::format("Expand macro '{}'",
                                 unit.token_spelling(focus.location(name.range.begin))),
            .kind = action_kind::expand_macro,
            .edits = {{{begin, end}, std::move(text)}},
        });
        return;
    }
}

}  // namespace clice::feature::action
