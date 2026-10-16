module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.feature;
import :semantic.decls;
import :semantic.documentation;
import :semantic.selection;
import :semantic.semantics;
import :support.text;
import :syntax.lexer;

namespace clice::feature {

namespace {

/// The ranges around one offset an expanding selection may step through,
/// in no particular order: nest() picks the chain. A loose range joins it
/// only where it nests with the others.
struct Steps {
    std::uint32_t offset;

    /// The token the selection starts from when the offset sits between
    /// two (start_token): every step holds it.
    std::optional<LocalSourceRange> start;

    std::vector<LocalSourceRange> ranges;
    std::vector<LocalSourceRange> loose;

    void add(LocalSourceRange range) {
        if(range.begin < range.end && range.touches(offset)) {
            ranges.push_back(range);
        }
    }

    void add_loose(LocalSourceRange range) {
        if(range.begin < range.end && range.touches(offset)) {
            loose.push_back(range);
        }
    }
};

/// The most steps one offset gets. Only generated code nests deeper (a
/// sum of thousands of terms); its middle steps go.
constexpr std::size_t step_limit = 128;

/// The steps innermost first, each strictly containing the one before. Of
/// two ranges that overlap without nesting, the smaller one stays; a loose
/// one goes between the two steps it nests between, else nowhere.
std::vector<LocalSourceRange> nest(Steps steps) {
    auto by_length = [](const LocalSourceRange& range) {
        return std::pair(range.length(), range.begin);
    };
    auto holds_start = [&](const LocalSourceRange& range) {
        return !steps.start || range.contains(*steps.start);
    };
    std::ranges::sort(steps.ranges, {}, by_length);
    std::vector<LocalSourceRange> chain;
    for(auto& range: steps.ranges) {
        if(holds_start(range) &&
           (chain.empty() || (range != chain.back() && range.contains(chain.back())))) {
            chain.push_back(range);
        }
    }
    std::ranges::sort(steps.loose, {}, by_length);
    for(auto& range: steps.loose) {
        auto outer = std::ranges::partition_point(chain, [&](const LocalSourceRange& step) {
            return range.contains(step);
        });
        bool fits = holds_start(range) && (outer == chain.begin() || *std::prev(outer) != range) &&
                    (outer == chain.end() || (*outer != range && outer->contains(range)));
        if(fits) {
            chain.insert(outer, range);
        }
    }
    if(chain.empty()) {
        chain.push_back({steps.offset, steps.offset});
    }
    if(chain.size() > step_limit) {
        chain.erase(chain.begin() + step_limit / 2, chain.end() - step_limit / 2);
    }
    return chain;
}

LocalSourceRange hull(LocalSourceRange left, LocalSourceRange right) {
    return {std::min(left.begin, right.begin), std::max(left.end, right.end)};
}

/// `range` shrunk to its first and last non-blank characters.
LocalSourceRange trimmed(llvm::StringRef content, LocalSourceRange range) {
    auto text = content.slice(range.begin, range.end);
    auto front = text.find_first_not_of(" \t\r\n\f\v");
    if(front == llvm::StringRef::npos) {
        return {range.begin, range.begin};
    }
    auto back = text.find_last_not_of(" \t\r\n\f\v");
    return {static_cast<std::uint32_t>(range.begin + front),
            static_cast<std::uint32_t>(range.begin + back + 1)};
}

/// A comment's text past its opening marker, documentation markers
/// (`///`, `//!`, `/**`, `/*!`, each also with `<`) included, and before a
/// block's closing `*/`.
LocalSourceRange comment_text(llvm::StringRef content, LocalSourceRange comment) {
    constexpr llvm::StringLiteral markers[] =
        {"///<", "//!<", "/**<", "/*!<", "///", "//!", "/**", "/*!", "//", "/*"};
    auto text = content.slice(comment.begin, comment.end);
    std::size_t begin = llvm::find_if(markers, [&](llvm::StringRef marker) {
                            return text.starts_with(marker);
                        })->size();
    std::size_t end = text.size();
    if(text.starts_with("/*") && text.ends_with("*/")) {
        end = std::max(begin, end - 2);
    }
    return trimmed(content,
                   {static_cast<std::uint32_t>(comment.begin + begin),
                    static_cast<std::uint32_t>(comment.begin + end)});
}

bool quoted(TokenKind kind) {
    return clang::tok::isStringLiteral(kind) || kind == clang::tok::header_name ||
           llvm::is_contained({clang::tok::char_constant,
                               clang::tok::wide_char_constant,
                               clang::tok::utf8_char_constant,
                               clang::tok::utf16_char_constant,
                               clang::tok::utf32_char_constant},
                              kind);
}

/// The characters between a literal's quotes: past an encoding prefix and
/// a raw string's delimiter, before a user-defined suffix.
LocalSourceRange quoted_text(llvm::StringRef content, const Token& token) {
    auto text = token.text(content);
    std::size_t begin = 1;
    std::size_t end = text.size() - 1;
    if(token.kind != clang::tok::header_name) {
        auto quote = text.find_first_of("\"'");
        begin = quote + 1;
        end = text.rfind(text[quote]);
        if(text[quote] == '"' && quote > 0 && text[quote - 1] == 'R') {
            begin = text.find('(', quote) + 1;
            end = text.rfind(')', end);
        }
    }
    return {static_cast<std::uint32_t>(token.range.begin + begin),
            static_cast<std::uint32_t>(token.range.begin + end)};
}

TokenKind closer(TokenKind opener) {
    switch(opener) {
        case clang::tok::l_paren: return clang::tok::r_paren;
        case clang::tok::l_square: return clang::tok::r_square;
        case clang::tok::l_brace: return clang::tok::r_brace;
        default: return clang::tok::unknown;
    }
}

/// Bracket pairs over a token stream. Brackets pair within their scope —
/// a directive line, a stretch the preprocessor skipped — and outside any
/// across the file; a closer without an opener of its kind (a
/// conditional's branches can leave both unbalanced) closes nothing, and
/// the openers a scope leaves open close with it.
struct Brackets {
    struct Open {
        TokenKind closer;
        LocalSourceRange range;
    };

    llvm::SmallVector<Open> opens;
    llvm::SmallVector<std::size_t, 2> scopes;

    void enter() {
        scopes.push_back(opens.size());
    }

    void leave() {
        opens.truncate(scopes.pop_back_val());
    }

    /// The opener `token` closes, if it is a closer that pairs.
    std::optional<LocalSourceRange> visit(const Token& token) {
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
};

/// The ranges a raw lex of the file shows, the same for every offset: the
/// names and literals, a literal's text, a comment's text and the comment,
/// the inside of each bracket pair and the pair, a directive line, a
/// module or import declaration; loose, the inside of each branch of a
/// conditional and the whole conditional, a run of whole-line comments.
struct TextSteps {
    std::vector<LocalSourceRange> ranges;
    std::vector<LocalSourceRange> loose;

    /// Where the closer of the bracket opening at each offset ends.
    llvm::DenseMap<std::uint32_t, std::uint32_t> closers;
};

/// The first token at or after `offset` that is not a comment.
const Token* token_after(llvm::ArrayRef<Token> tokens, std::uint32_t offset) {
    auto it = std::ranges::find_if(
        std::ranges::lower_bound(tokens, offset, {}, [](const Token& t) { return t.range.begin; }),
        tokens.end(),
        [](const Token& t) { return t.kind != clang::tok::comment; });
    return it == tokens.end() ? nullptr : &*it;
}

/// `skipped` flags the tokens the preprocessor skipped, when known.
TextSteps scan_text(llvm::StringRef content,
                    llvm::ArrayRef<Token> tokens,
                    llvm::ArrayRef<bool> skipped) {
    TextSteps text;
    Brackets brackets;
    std::optional<LocalSourceRange> directive;
    llvm::StringRef directive_name;
    /// The open conditionals: where each starts, and where the text of its
    /// current branch does.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>> conditionals;
    std::optional<std::uint32_t> declaration;
    bool skipping = false;
    std::optional<LocalSourceRange> comments;
    std::size_t comment_count = 0;

    auto close_comments = [&] {
        if(comments && comment_count > 1) {
            text.loose.push_back(*comments);
        }
        comments.reset();
    };

    auto close_directive = [&] {
        text.ranges.push_back(*directive);
        brackets.leave();
        if(llvm::is_contained({"if", "ifdef", "ifndef"}, directive_name)) {
            conditionals.emplace_back(directive->begin, directive->end);
        } else if(!conditionals.empty() &&
                  llvm::is_contained({"elif", "elifdef", "elifndef", "else", "endif"},
                                     directive_name)) {
            auto& [begin, branch] = conditionals.back();
            text.loose.push_back(trimmed(content, {branch, directive->begin}));
            branch = directive->end;
            if(directive_name == "endif") {
                text.loose.push_back({begin, directive->end});
                conditionals.pop_back();
            }
        }
        directive.reset();
    };

    for(std::size_t i = 0; i < tokens.size(); i += 1) {
        const auto& token = tokens[i];
        if(token.kind == clang::tok::comment) {
            text.ranges.push_back(comment_text(content, token.range));
            text.ranges.push_back(token.range);
            if(directive) {
                directive->end = token.range.end;
            }
            bool joins = comments && token.is_at_start_of_line &&
                         content.slice(comments->end, token.range.begin).count('\n') == 1;
            if(joins) {
                comments->end = token.range.end;
                comment_count += 1;
                continue;
            }
            close_comments();
            if(token.is_at_start_of_line) {
                comments = token.range;
                comment_count = 1;
            }
            continue;
        }
        close_comments();

        if(token.is_eod()) {
            if(directive) {
                close_directive();
            }
            continue;
        }
        if(!skipped.empty() && skipped[i] != skipping && !directive) {
            skipping = skipped[i];
            if(skipping) {
                brackets.enter();
            } else {
                brackets.leave();
            }
        }
        if(token.is_directive_hash()) {
            directive = token.range;
            directive_name = {};
            brackets.enter();
        } else if(directive) {
            directive->end = token.range.end;
            if(token.is_pp_keyword) {
                directive_name = token.text(content);
            }
        }

        // `[export] module` and `[export] import` begin a declaration only
        // at the start of a line ([cpp.module], [cpp.import]).
        if(token.is_at_start_of_line) {
            declaration.reset();
            auto word = token.text(content);
            if(word == "export") {
                if(const auto* next = token_after(tokens, token.range.end)) {
                    word = next->text(content);
                }
            }
            if(!directive && brackets.opens.empty() && token.is_identifier() &&
               (word == "module" || word == "import")) {
                declaration = token.range.begin;
            }
        }
        if(declaration && token.kind == clang::tok::semi) {
            text.ranges.push_back({*declaration, token.range.end});
            declaration.reset();
        }

        if(token.is_identifier() || clang::tok::isLiteral(token.kind)) {
            text.ranges.push_back(token.range);
        }
        if(quoted(token.kind)) {
            text.ranges.push_back(quoted_text(content, token));
        }
        if(auto open = brackets.visit(token)) {
            text.ranges.push_back(trimmed(content, {open->end, token.range.begin}));
            text.ranges.push_back({open->begin, token.range.end});
            text.closers.try_emplace(open->begin, token.range.end);
        }
    }
    close_comments();
    if(directive) {
        close_directive();
    }
    return text;
}

/// The token a cursor between two starts from: a name or literal over
/// punctuation, punctuation over a `;`, anything over a comment, else the
/// right one. None when the cursor touches only one token, or none.
std::optional<LocalSourceRange> start_token(llvm::ArrayRef<Token> tokens, std::uint32_t offset) {
    auto right =
        std::ranges::lower_bound(tokens, offset, {}, [](const Token& t) { return t.range.begin; });
    if(right == tokens.begin() || right == tokens.end() || right->range.begin != offset ||
       right->is_eod()) {
        return std::nullopt;
    }
    auto left = std::prev(right);
    if(left->range.end != offset || left->is_eod()) {
        return std::nullopt;
    }
    auto rank = [](const Token& token) {
        if(token.kind == clang::tok::comment) {
            return 0;
        }
        if(token.kind == clang::tok::semi) {
            return 1;
        }
        return clang::tok::getPunctuatorSpelling(token.kind) ? 2 : 3;
    };
    return rank(*left) > rank(*right) ? left->range : right->range;
}

/// The text of the line of a block comment over several lines `offset` is
/// on, past the `*` such a comment's lines begin with.
std::optional<LocalSourceRange> comment_line(llvm::StringRef content,
                                             llvm::ArrayRef<Token> tokens,
                                             std::uint32_t offset) {
    auto it =
        std::ranges::lower_bound(tokens, offset, {}, [](const Token& t) { return t.range.end; });
    if(it == tokens.end() || it->kind != clang::tok::comment || it->range.begin > offset) {
        return std::nullopt;
    }
    auto comment = it->text(content);
    if(!comment.starts_with("/*") || !comment.contains('\n')) {
        return std::nullopt;
    }
    auto inside = comment_text(content, it->range);
    auto begin = std::max(inside.begin, line_begin(content, offset));
    auto end = std::min(inside.end, line_end(content, offset));
    if(begin >= end) {
        return std::nullopt;
    }
    auto line = content.slice(begin, end).ltrim(" \t");
    if(line.starts_with("*") && !line.starts_with("*/")) {
        line = line.drop_front();
    }
    return trimmed(content, {static_cast<std::uint32_t>(line.data() - content.data()), end});
}

std::vector<Token> lex(llvm::StringRef content, const clang::LangOptions& lang_opts) {
    std::vector<Token> tokens;
    Lexer lexer(content, {.keep_comments = true, .lang_opts = &lang_opts});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        tokens.push_back(token);
    }
    return tokens;
}

/// Per raw token, whether the preprocessor skipped it; a token the unit's
/// spelled tokens do not start at (a comment, a directive's end) shares
/// the flag of the one before.
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

/// The steps of one offset only the text shows.
void add_text(llvm::StringRef content,
              llvm::ArrayRef<Token> tokens,
              const TextSteps& text,
              Steps& steps) {
    steps.start = start_token(tokens, steps.offset);
    for(auto range: text.ranges) {
        steps.add(range);
    }
    for(auto range: text.loose) {
        steps.add_loose(range);
    }
    if(auto line = comment_line(content, tokens, steps.offset)) {
        steps.add(*line);
    }
}

/// The tokens the selection starts from: the one the cursor touches — a
/// name or literal over punctuation, the right one of two alike, a macro
/// invocation's name for its parentheses — else the nearest on either
/// side (the cursor is in blank space, a comment or a qualifier). The
/// selection never counts `;`, comments, qualifiers and what was
/// preprocessed away, so they anchor nothing.
llvm::SmallVector<LocalSourceRange, 2> anchors(CompilationUnit& unit, std::uint32_t offset) {
    const auto& semantics = unit.get<Semantics>();
    auto tokens = semantics.spelled_tokens();
    auto range_of = [&](std::uint32_t i) {
        auto begin = semantics.token_offset(i);
        return LocalSourceRange{begin, begin + tokens[i].length()};
    };
    auto usable = [&](std::uint32_t i) {
        return !should_ignore_token(tokens[i]) && !semantics.token_preprocessed_away(i);
    };
    auto word = [&](std::uint32_t i) {
        return clang::tok::getPunctuatorSpelling(tokens[i].kind()) == nullptr;
    };

    auto count = static_cast<std::uint32_t>(tokens.size());
    auto indices = std::views::iota(0u, count);
    auto first = static_cast<std::uint32_t>(
        std::ranges::partition_point(indices,
                                     [&](std::uint32_t i) { return range_of(i).end < offset; }) -
        indices.begin());

    std::optional<std::uint32_t> touched;
    for(auto i = first; i < count && range_of(i).begin <= offset; i += 1) {
        if(usable(i) && (!touched || word(i) || !word(*touched))) {
            touched = i;
        }
    }
    if(touched) {
        // The parentheses and commas of a macro invocation stand for no
        // node; its name stands for what the invocation expands to.
        if(semantics.owners(*touched).empty()) {
            const auto& map = unit.focus_file().tokens();
            auto expansions = map.expansions_overlapping(tokens.slice(*touched, 1));
            if(!expansions.empty() && !expansions.front().expanded.empty()) {
                const auto& name = expansions.front().spelled.front();
                return {range_of(static_cast<std::uint32_t>(&name - tokens.data()))};
            }
        }
        return {range_of(*touched)};
    }

    llvm::SmallVector<LocalSourceRange, 2> sides;
    auto before = first;
    while(before > 0 && !usable(before - 1)) {
        before -= 1;
    }
    if(before > 0) {
        sides.push_back(range_of(before - 1));
    }
    auto after = first;
    while(after < count && !usable(after)) {
        after += 1;
    }
    if(after < count) {
        sides.push_back(range_of(after));
    }
    return sides;
}

/// A node's range in the focus file: the tokens a macro argument or a
/// whole expansion spells for it, else the invocations it spans.
std::optional<LocalSourceRange> focus_range(CompilationUnit& unit, clang::SourceRange range) {
    if(range.isInvalid()) {
        return std::nullopt;
    }
    auto focus = unit.focus_file();
    if(auto written = focus.written_range(range)) {
        return written;
    }
    return focus.produced_range(range);
}

/// An attribute's range with its argument list: `alignas` alone is the
/// range of `alignas(16)`.
LocalSourceRange attribute_range(llvm::ArrayRef<Token> tokens,
                                 const TextSteps& text,
                                 LocalSourceRange range) {
    if(const auto* next = token_after(tokens, range.end);
       next && next->kind == clang::tok::l_paren) {
        if(auto it = text.closers.find(next->range.begin); it != text.closers.end()) {
            range.end = it->second;
        }
    }
    return range;
}

/// Where an attribute is written: with its argument list and the `[[ ]]`,
/// `__attribute__(( ))` or `__declspec( )` around it.
std::optional<LocalSourceRange> attribute_extent(CompilationUnit& unit,
                                                 llvm::StringRef content,
                                                 llvm::ArrayRef<Token> tokens,
                                                 const TextSteps& text,
                                                 const clang::Attr& attr) {
    auto& context = unit.context();
    auto begin = decls::attribute_begin(attr, context.getSourceManager(), context.getLangOpts());
    auto range = focus_range(unit, clang::SourceRange(begin, attr.getRange().getEnd()));
    if(!range) {
        return std::nullopt;
    }
    *range = attribute_range(tokens, text, *range);
    auto it = std::ranges::lower_bound(tokens, range->begin, {}, [](const Token& t) {
        return t.range.begin;
    });
    const Token* opener = nullptr;
    if(it != tokens.end() && it->range.begin == range->begin && it->kind == clang::tok::l_square) {
        opener = &*it;
    } else {
        llvm::SmallVector<const Token*, 3> before;
        while(it != tokens.begin() && before.size() < 3) {
            it = std::prev(it);
            if(it->kind != clang::tok::comment) {
                before.push_back(&*it);
            }
        }
        auto is = [&](std::size_t back, TokenKind kind) {
            return back < before.size() && before[back]->kind == kind;
        };
        auto spelled = [&](std::size_t back, llvm::StringRef word) {
            return back < before.size() && before[back]->text(content) == word;
        };
        if(is(0, clang::tok::l_paren) && is(1, clang::tok::l_paren) &&
           spelled(2, "__attribute__")) {
            opener = before[1];
            range->begin = before[2]->range.begin;
        } else if(is(0, clang::tok::l_paren) && spelled(1, "__declspec")) {
            opener = before[0];
            range->begin = before[1]->range.begin;
        }
    }
    if(opener) {
        if(auto close = text.closers.find(opener->range.begin); close != text.closers.end()) {
            range->end = std::max(range->end, close->second);
        }
    }
    return range;
}

/// A node's step: a type's with its qualifier, a declaration's with the
/// attributes written on it, an attribute's with its argument list.
std::optional<LocalSourceRange> node_range(CompilationUnit& unit,
                                           llvm::StringRef content,
                                           llvm::ArrayRef<Token> tokens,
                                           const TextSteps& text,
                                           const SelectionTree::Node& node) {
    // The namespace a nested definition names last spans from its `::`
    // (`namespace a::b {`): no step of its own.
    if(const auto* scope = node.get<clang::NamespaceDecl>(); scope && scope->isNested()) {
        return std::nullopt;
    }
    // The tree gives a type the range its qualifier and keyword are left
    // out of (`typename ns::Box<T>::type` is `type`); they are its children.
    const auto* type = node.get<clang::TypeLoc>();
    auto range = focus_range(unit, type ? type->getSourceRange() : node.source_range());
    if(!range) {
        return std::nullopt;
    }
    if(const auto* decl = node.get<clang::Decl>()) {
        for(const auto* attr: decl->attrs()) {
            auto written = attr->getRange();
            if(attr->isImplicit() || attr->isInherited() || written.isInvalid() ||
               decls::applied_by_pragma(decl->getASTContext(), written.getBegin())) {
                continue;
            }
            if(auto extent = attribute_extent(unit, content, tokens, text, *attr)) {
                range = hull(*range, *extent);
            }
        }
    } else if(node.get<clang::Attr>()) {
        range = attribute_range(tokens, text, *range);
    }
    return range;
}

/// Whether the `;` following a node ends the statement or declaration the
/// node is: a statement of a block, the body or init-statement of a
/// control statement, a label's statement; a declaration outside a
/// declaration statement. A condition, an expression a `return` ends with
/// and a body a declaration or a lambda owns are no statements of their
/// own.
bool ends_statement(const SelectionTree::Node& node) {
    const auto* parent = node.parent;
    if(node.get<clang::Decl>()) {
        return !parent->get<clang::DeclStmt>();
    }
    const auto* statement = node.get<clang::Stmt>();
    const auto* owner = parent->get<clang::Stmt>();
    if(!statement || !owner) {
        return false;
    }
    if(llvm::isa<clang::CompoundStmt>(owner)) {
        return true;
    }
    llvm::SmallVector<const clang::Stmt*, 3> bodies;
    if(const auto* branch = llvm::dyn_cast<clang::IfStmt>(owner)) {
        bodies = {branch->getInit(), branch->getThen(), branch->getElse()};
    } else if(const auto* loop = llvm::dyn_cast<clang::ForStmt>(owner)) {
        bodies = {loop->getInit(), loop->getBody()};
    } else if(const auto* loop = llvm::dyn_cast<clang::CXXForRangeStmt>(owner)) {
        bodies = {loop->getInit(), loop->getBody()};
    } else if(const auto* loop = llvm::dyn_cast<clang::WhileStmt>(owner)) {
        bodies = {loop->getBody()};
    } else if(const auto* loop = llvm::dyn_cast<clang::DoStmt>(owner)) {
        bodies = {loop->getBody()};
    } else if(const auto* choice = llvm::dyn_cast<clang::SwitchStmt>(owner)) {
        bodies = {choice->getInit(), choice->getBody()};
    } else if(const auto* label = llvm::dyn_cast<clang::SwitchCase>(owner)) {
        bodies = {label->getSubStmt()};
    } else if(const auto* label = llvm::dyn_cast<clang::LabelStmt>(owner)) {
        bodies = {label->getSubStmt()};
    } else if(const auto* attributed = llvm::dyn_cast<clang::AttributedStmt>(owner)) {
        bodies = {attributed->getSubStmt()};
    }
    return llvm::is_contained(bodies, statement);
}

/// The angle brackets of the explicit template arguments a name carries
/// (`f<int>`, `object.f<int>`, dependent and unresolved names alike).
template <typename... Names>
std::optional<clang::SourceRange> explicit_arguments(const clang::Expr* expr) {
    std::optional<clang::SourceRange> angles;
    (
        [&] {
            if(const auto* name = llvm::dyn_cast<Names>(expr);
               name && name->hasExplicitTemplateArgs()) {
                angles = clang::SourceRange(name->getLAngleLoc(), name->getRAngleLoc());
            }
        }(),
        ...);
    return angles;
}

/// The angle brackets of the template parameter lists and template
/// argument lists a node writes: a template-id's, a name's explicit
/// arguments, a template's parameters, a specialization's parameters and
/// arguments.
llvm::SmallVector<clang::SourceRange, 2> angles(const SelectionTree::Node& node) {
    llvm::SmallVector<clang::SourceRange, 2> lists;
    auto add_parameters = [&](const clang::TemplateParameterList* parameters) {
        lists.emplace_back(parameters->getLAngleLoc(), parameters->getRAngleLoc());
    };
    auto add_arguments = [&](const clang::ASTTemplateArgumentListInfo* arguments) {
        if(arguments) {
            lists.emplace_back(arguments->getLAngleLoc(), arguments->getRAngleLoc());
        }
    };
    if(const auto* loc = node.get<clang::TypeLoc>()) {
        if(auto id = loc->getAs<clang::TemplateSpecializationTypeLoc>()) {
            lists.emplace_back(id.getLAngleLoc(), id.getRAngleLoc());
        }
    } else if(const auto* expr = node.get<clang::Expr>()) {
        if(auto arguments = explicit_arguments<clang::DeclRefExpr,
                                               clang::MemberExpr,
                                               clang::OverloadExpr,
                                               clang::DependentScopeDeclRefExpr,
                                               clang::CXXDependentScopeMemberExpr>(expr)) {
            lists.push_back(*arguments);
        }
    } else if(const auto* decl = node.get<clang::Decl>()) {
        if(const auto* temp = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            add_parameters(temp->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            add_parameters(partial->getTemplateParameters());
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            add_parameters(partial->getTemplateParameters());
        }
        if(const auto* spec = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(decl)) {
            add_arguments(spec->getTemplateArgsAsWritten());
        } else if(const auto* spec = llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(decl)) {
            add_arguments(spec->getTemplateArgsAsWritten());
        } else if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
            add_arguments(function->getTemplateSpecializationArgsAsWritten());
        }
    }
    return lists;
}

/// The declaration `decl` shares with the declarators after it
/// (`int a = 1, b = 2`), from its range through the last of them.
std::optional<LocalSourceRange> declarators(CompilationUnit& unit,
                                            const clang::Decl& decl,
                                            LocalSourceRange range) {
    const clang::Decl* last = &decl;
    for(const auto* next = decl.getNextDeclInContext(); next; next = next->getNextDeclInContext()) {
        if(next->isImplicit()) {
            continue;
        }
        if(next->getBeginLoc() != decl.getBeginLoc()) {
            break;
        }
        last = next;
    }
    if(last == &decl) {
        return std::nullopt;
    }
    auto group = focus_range(unit, last->getSourceRange());
    return group ? std::optional(hull(range, *group)) : std::nullopt;
}

/// A function's signature — from its return type, or from the name a
/// constructor's parameters follow, through the parameters and what is
/// written after them — and a constructor's initializer list.
void add_function(CompilationUnit& unit, const clang::FunctionDecl& function, Steps& steps) {
    if(auto type = function.getFunctionTypeLoc()) {
        auto name = function.getQualifierLoc() ? function.getQualifierLoc().getBeginLoc()
                                               : function.getNameInfo().getBeginLoc();
        auto written = focus_range(unit, type.getSourceRange());
        auto named = focus_range(unit, name);
        if(written && named) {
            steps.add({std::min(written->begin, named->begin), written->end});
        }
    }
    const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&function);
    if(!constructor) {
        return;
    }
    std::optional<LocalSourceRange> list;
    for(const auto* init: constructor->inits()) {
        if(!init->isWritten()) {
            continue;
        }
        if(auto written = focus_range(unit, init->getSourceRange())) {
            list = list ? hull(*list, *written) : *written;
        }
    }
    if(list) {
        steps.add(*list);
    }
}

/// The section of a switch's body a statement of it belongs to: from the
/// `case` or `default` label before it through the last statement before
/// the next label. None for a label leading no statement but its own.
std::optional<clang::SourceRange> case_section(const SelectionTree::Node& node) {
    const auto* statement = node.get<clang::Stmt>();
    const auto* body = statement ? node.parent->get<clang::CompoundStmt>() : nullptr;
    if(!body) {
        return std::nullopt;
    }
    llvm::ArrayRef<clang::Stmt*> statements(body->body_begin(), body->body_end());
    auto first = static_cast<std::size_t>(llvm::find(statements, statement) - statements.begin());
    while(first > 0 && !llvm::isa<clang::SwitchCase>(statements[first])) {
        first -= 1;
    }
    if(!llvm::isa<clang::SwitchCase>(statements[first])) {
        return std::nullopt;
    }
    auto last = first;
    while(last + 1 < statements.size() && !llvm::isa<clang::SwitchCase>(statements[last + 1])) {
        last += 1;
    }
    if(last == first) {
        return std::nullopt;
    }
    return clang::SourceRange(statements[first]->getBeginLoc(), statements[last]->getEndLoc());
}

/// The ranges of the AST nodes around the cursor — the ancestors of each
/// anchor that contain it — each statement also with the `;` that ends
/// it, each template list also with the inside of its angle brackets,
/// each statement of a switch with its `case` section, a declaration also
/// with the declarators sharing it and with the comment documenting it.
void add_ast(CompilationUnit& unit,
             llvm::StringRef content,
             llvm::ArrayRef<Token> tokens,
             const TextSteps& text,
             Steps& steps) {
    auto semicolon_after = [&](std::uint32_t end) -> std::optional<std::uint32_t> {
        const auto* next = token_after(tokens, end);
        if(!next || next->kind != clang::tok::semi) {
            return std::nullopt;
        }
        return next->range.end;
    };
    auto focus = unit.focus_file().file();

    for(auto anchor: anchors(unit, steps.offset)) {
        auto tree = SelectionTree::create_right(unit, anchor);
        for(const auto* node = tree.common_ancestor(); node && node->parent; node = node->parent) {
            auto range = node_range(unit, content, tokens, text, *node);
            if(!range) {
                continue;
            }
            bool statement = ends_statement(*node);
            auto with_semicolon = [&](LocalSourceRange step) {
                steps.add(step);
                if(auto end = semicolon_after(step.end); statement && end) {
                    step.end = *end;
                    steps.add(step);
                }
                return step;
            };
            auto extent = with_semicolon(*range);
            for(auto brackets: angles(*node)) {
                auto left = focus_range(unit, brackets.getBegin());
                auto right = focus_range(unit, brackets.getEnd());
                if(left && right) {
                    steps.add(trimmed(content, {left->end, right->begin}));
                    steps.add({left->begin, right->end});
                }
            }
            if(auto section = case_section(*node)) {
                if(auto written = focus_range(unit, *section)) {
                    steps.add(
                        {written->begin, semicolon_after(written->end).value_or(written->end)});
                }
            }
            const auto* decl = node->get<clang::Decl>();
            if(!decl) {
                continue;
            }
            if(auto group = declarators(unit, *decl, *range)) {
                extent = with_semicolon(*group);
            }
            if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                add_function(unit, *function, steps);
            }
            if(node->parent->get<clang::DeclStmt>()) {
                if(auto whole = focus_range(unit, node->parent->source_range())) {
                    extent = hull(extent, *whole);
                }
            }
            if(auto comments = doc_comment_range(unit, decl);
               comments && comments->first == focus) {
                steps.add(hull(extent, comments->second));
            }
        }
    }
}

}  // namespace

auto selection_ranges(CompilationUnit& unit, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto content = unit.focus_file().content();
    auto tokens = lex(content, unit.lang_options());
    auto text = scan_text(content, tokens, skipped_tokens(unit, tokens));
    std::vector<std::vector<LocalSourceRange>> result;
    for(auto offset: offsets) {
        Steps steps{.offset = offset};
        add_text(content, tokens, text, steps);
        add_ast(unit, content, tokens, text, steps);
        result.push_back(nest(std::move(steps)));
    }
    return result;
}

auto lexical_selection_ranges(llvm::StringRef content,
                              const clang::LangOptions& lang_opts,
                              llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto tokens = lex(content, lang_opts);
    auto text = scan_text(content, tokens, {});
    std::vector<std::vector<LocalSourceRange>> result;
    for(auto offset: offsets) {
        Steps steps{.offset = offset};
        add_text(content, tokens, text, steps);
        result.push_back(nest(std::move(steps)));
    }
    return result;
}

auto selection_range_to_protocol(llvm::ArrayRef<LocalSourceRange> ranges,
                                 const PositionMap& map,
                                 PositionEncoding encoding) -> protocol::SelectionRange {
    std::unique_ptr<protocol::SelectionRange> parent;
    for(const auto& range: llvm::reverse(ranges)) {
        auto node = std::make_unique<protocol::SelectionRange>();
        node->range = *map.range(range, encoding);
        node->parent = std::move(parent);
        parent = std::move(node);
    }
    return std::move(*parent);
}

}  // namespace clice::feature
