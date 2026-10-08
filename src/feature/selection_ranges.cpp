module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :compile.selection;
import :compile.semantics;
import :feature.feature;
import :syntax.lexer;

namespace clice::feature {

namespace {

/// The ranges around one offset an expanding selection may step through,
/// in no particular order: nest() picks the chain.
struct Steps {
    std::uint32_t offset;
    std::vector<LocalSourceRange> ranges;

    void add(LocalSourceRange range) {
        if(range.begin < range.end && range.contains(offset)) {
            ranges.push_back(range);
        }
    }
};

/// The steps innermost first, each strictly containing the one before. Of
/// two ranges that overlap without nesting, the smaller one stays.
std::vector<LocalSourceRange> nest(Steps steps) {
    std::ranges::sort(steps.ranges, {}, [](const LocalSourceRange& range) {
        return std::pair(range.length(), range.begin);
    });
    std::vector<LocalSourceRange> chain;
    for(auto& range: steps.ranges) {
        if(chain.empty() || (range != chain.back() && range.contains(chain.back()))) {
            chain.push_back(range);
        }
    }
    if(chain.empty()) {
        chain.push_back({steps.offset, steps.offset});
    }
    return chain;
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

/// A comment's text past its markers: `//`, `/*` and the `/` `*` `!` of
/// documentation comments, a block's closing `*/`.
LocalSourceRange comment_text(llvm::StringRef content, LocalSourceRange comment) {
    auto text = content.slice(comment.begin, comment.end);
    std::size_t begin = 2;
    while(begin < text.size() && llvm::is_contained({'/', '*', '!'}, text[begin])) {
        begin += 1;
    }
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

/// The ranges a raw lex knows: the name or literal under the cursor, a
/// literal's text, a comment's text, the comment and the run of
/// whole-line comments it belongs to, the inside of each bracket pair
/// around the cursor and the pair itself, a directive line. `skipped`
/// flags the tokens the preprocessor skipped, when known.
void add_lexical(llvm::StringRef content,
                 llvm::ArrayRef<Token> tokens,
                 llvm::ArrayRef<bool> skipped,
                 Steps& steps) {
    Brackets brackets;
    std::optional<LocalSourceRange> directive;
    bool skipping = false;
    std::optional<LocalSourceRange> comments;
    std::size_t comment_count = 0;

    auto close_comments = [&] {
        if(comments && comment_count > 1) {
            steps.add(*comments);
        }
        comments.reset();
    };

    for(std::size_t i = 0; i < tokens.size(); i += 1) {
        const auto& token = tokens[i];
        if(token.kind == clang::tok::comment) {
            steps.add(comment_text(content, token.range));
            steps.add(token.range);
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
                steps.add(*directive);
                brackets.leave();
                directive.reset();
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
            brackets.enter();
        } else if(directive) {
            directive->end = token.range.end;
        }

        if(token.is_identifier() || clang::tok::isLiteral(token.kind)) {
            steps.add(token.range);
        }
        if(quoted(token.kind)) {
            steps.add(quoted_text(content, token));
        }
        if(auto open = brackets.visit(token)) {
            steps.add(trimmed(content, {open->end, token.range.begin}));
            steps.add({open->begin, token.range.end});
        }
    }
    close_comments();
    if(directive) {
        steps.add(*directive);
    }
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
llvm::SmallVector<bool, 0> skipped_tokens(CompilationUnitRef unit, llvm::ArrayRef<Token> tokens) {
    const auto& semantics = unit.semantics();
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

/// The selection the cursor stands in: the token it touches — a name or
/// literal over punctuation, the right one of two alike — else the
/// stretch between the tokens around it (a comment, blank space). Tokens
/// the selection never counts (`;`, comments, preprocessed away) do not
/// anchor it.
std::optional<SelectionTree> selection_at(CompilationUnitRef unit, std::uint32_t offset) {
    const auto& semantics = unit.semantics();
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
        return SelectionTree::create_right(unit, range_of(*touched));
    }

    auto before = first;
    while(before > 0 && !usable(before - 1)) {
        before -= 1;
    }
    auto after = first;
    while(after < count && !usable(after)) {
        after += 1;
    }
    if(before == 0 || after == count) {
        return std::nullopt;
    }
    return SelectionTree::create_right(unit, {range_of(before - 1).begin, range_of(after).end});
}

/// A node's range in the main file: the tokens a macro argument or a
/// whole expansion spells for it, else the invocations it spans.
std::optional<LocalSourceRange> main_file_range(CompilationUnitRef unit, clang::SourceRange range) {
    if(range.isInvalid()) {
        return std::nullopt;
    }
    if(auto spelled = unit.spelled_tokens(range); !spelled.empty()) {
        return LocalSourceRange{
            unit.file_offset(spelled.front().location()),
            unit.file_offset(spelled.back().location()) + spelled.back().length(),
        };
    }
    auto [fid, local] = unit.decompose_expansion_range(range);
    if(fid != unit.main_file()) {
        return std::nullopt;
    }
    return local;
}

/// Whether the `;` following a node ends the statement or declaration the
/// node is: a statement, an expression among them included, in a statement
/// list or a control statement's body, a declaration outside a declaration
/// statement. An expression a `return` ends with is no statement of its
/// own, nor is a body a declaration or a lambda owns.
bool ends_statement(const SelectionTree::Node& node) {
    const auto* parent = node.parent;
    if(node.get<clang::Decl>()) {
        return !parent->get<clang::DeclStmt>();
    }
    if(!node.get<clang::Stmt>()) {
        return false;
    }
    const auto* statement = parent->get<clang::Stmt>();
    return statement && !llvm::isa<clang::Expr, clang::ReturnStmt, clang::CoreturnStmt>(statement);
}

/// The angle brackets of a template's parameter list or a template-id.
std::optional<clang::SourceRange> angles(const SelectionTree::Node& node) {
    if(const auto* loc = node.get<clang::TypeLoc>()) {
        if(auto id = loc->getAs<clang::TemplateSpecializationTypeLoc>()) {
            return clang::SourceRange(id.getLAngleLoc(), id.getRAngleLoc());
        }
    } else if(const auto* expr = node.get<clang::DeclRefExpr>()) {
        if(expr->hasExplicitTemplateArgs()) {
            return clang::SourceRange(expr->getLAngleLoc(), expr->getRAngleLoc());
        }
    } else if(const auto* decl = node.get<clang::Decl>()) {
        const clang::TemplateParameterList* params = nullptr;
        if(const auto* temp = llvm::dyn_cast<clang::TemplateDecl>(decl)) {
            params = temp->getTemplateParameters();
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(decl)) {
            params = partial->getTemplateParameters();
        } else if(const auto* partial =
                      llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(decl)) {
            params = partial->getTemplateParameters();
        }
        if(params) {
            return clang::SourceRange(params->getLAngleLoc(), params->getRAngleLoc());
        }
    }
    return std::nullopt;
}

/// The ranges of the AST nodes around the cursor, each statement also with
/// the `;` that ends it, each template-id and template parameter list
/// also with the inside of its angle brackets.
void add_ast(CompilationUnitRef unit,
             llvm::StringRef content,
             llvm::ArrayRef<Token> tokens,
             Steps& steps) {
    auto tree = selection_at(unit, steps.offset);
    if(!tree) {
        return;
    }
    auto semicolon_after = [&](std::uint32_t end) -> std::optional<std::uint32_t> {
        auto it = std::ranges::find_if(
            std::ranges::lower_bound(tokens, end, {}, [](const Token& t) { return t.range.begin; }),
            tokens.end(),
            [](const Token& t) { return t.kind != clang::tok::comment; });
        if(it == tokens.end() || it->kind != clang::tok::semi) {
            return std::nullopt;
        }
        return it->range.end;
    };

    for(const auto* node = tree->common_ancestor(); node && node->parent; node = node->parent) {
        auto range = main_file_range(unit, node->source_range());
        if(!range) {
            continue;
        }
        steps.add(*range);
        if(ends_statement(*node)) {
            if(auto end = semicolon_after(range->end)) {
                steps.add({range->begin, *end});
            }
        }
        if(auto brackets = angles(*node); brackets && brackets->isValid()) {
            auto left = main_file_range(unit, brackets->getBegin());
            auto right = main_file_range(unit, brackets->getEnd());
            if(left && right) {
                steps.add(trimmed(content, {left->end, right->begin}));
                steps.add({left->begin, right->end});
            }
        }
    }
}

}  // namespace

auto selection_ranges(CompilationUnitRef unit, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto content = unit.main_content();
    auto tokens = lex(content, unit.lang_options());
    auto skipped = skipped_tokens(unit, tokens);
    std::vector<std::vector<LocalSourceRange>> result;
    for(auto offset: offsets) {
        Steps steps{.offset = offset};
        add_lexical(content, tokens, skipped, steps);
        add_ast(unit, content, tokens, steps);
        result.push_back(nest(std::move(steps)));
    }
    return result;
}

auto lexical_selection_ranges(llvm::StringRef content,
                              const clang::LangOptions& lang_opts,
                              llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto tokens = lex(content, lang_opts);
    std::vector<std::vector<LocalSourceRange>> result;
    for(auto offset: offsets) {
        Steps steps{.offset = offset};
        add_lexical(content, tokens, {}, steps);
        result.push_back(nest(std::move(steps)));
    }
    return result;
}

auto selection_range_to_protocol(llvm::ArrayRef<LocalSourceRange> ranges, const PositionMap& map)
    -> protocol::SelectionRange {
    std::unique_ptr<protocol::SelectionRange> parent;
    for(const auto& range: llvm::reverse(ranges)) {
        auto node = std::make_unique<protocol::SelectionRange>();
        node->range = *map.to_range(range);
        node->parent = std::move(parent);
        parent = std::move(node);
    }
    return std::move(*parent);
}

}  // namespace clice::feature
