module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.selection_ranges;
import :semantic.decls;
import :semantic.documentation;
import :semantic.selection;
import :semantic.semantics;
import :support.text;
import :syntax.lexical_scan;
import :syntax.source_text;
import :syntax.token;

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
    std::size_t begin = std::ranges::find_if(markers, [&](llvm::StringRef marker) {
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
           std::ranges::contains(std::array{clang::tok::char_constant,
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

/// The ranges a raw lex of the file shows, the same for every offset: the
/// names and literals, a literal's text, a comment's text and the comment,
/// the inside of each bracket pair and the pair, an attribute list, a
/// directive line, a module or import declaration; loose, the inside of
/// each branch of a conditional and the whole conditional, a run of
/// whole-line comments.
struct TextSteps {
    std::vector<LocalSourceRange> ranges;
    std::vector<LocalSourceRange> loose;

    /// Where the closer of the bracket opening at each offset ends.
    llvm::DenseMap<std::uint32_t, std::uint32_t> closers;

    /// The attribute lists in the order they close: `[[…]]`,
    /// `__attribute__((…))`, `__declspec(…)`.
    std::vector<LocalSourceRange> attributes;
};

/// The first token at or after `offset` that is not a comment.
const Token* token_after(llvm::ArrayRef<Token> tokens, std::uint32_t offset) {
    auto it = std::ranges::find_if(
        std::ranges::lower_bound(tokens, offset, {}, [](const Token& t) { return t.range.begin; }),
        tokens.end(),
        [](const Token& t) { return t.kind != clang::tok::comment; });
    return it == tokens.end() ? nullptr : &*it;
}

/// The last token before `offset` that is not a comment.
const Token* token_before(llvm::ArrayRef<Token> tokens, std::uint32_t offset) {
    auto it =
        std::ranges::lower_bound(tokens, offset, {}, [](const Token& t) { return t.range.begin; });
    while(it != tokens.begin()) {
        it = std::prev(it);
        if(it->kind != clang::tok::comment) {
            return &*it;
        }
    }
    return nullptr;
}

/// `skipped` flags the tokens the preprocessor skipped, when known.
TextSteps scan_text(llvm::StringRef content,
                    llvm::ArrayRef<Token> tokens,
                    llvm::ArrayRef<bool> skipped) {
    TextSteps text;
    BracketPairs brackets;
    std::optional<LocalSourceRange> directive;
    llvm::StringRef directive_name;
    /// The open conditionals: where each starts, and where the text of its
    /// current branch does.
    llvm::SmallVector<std::pair<std::uint32_t, std::uint32_t>> conditionals;
    std::optional<std::uint32_t> declaration;
    std::optional<LocalSourceRange> comments;
    std::size_t comment_count = 0;

    auto close_comments = [&] {
        if(comments && comment_count > 1) {
            text.loose.push_back(*comments);
        }
        comments.reset();
    };

    auto close_directive = [&] {
        using enum LexicalInfo::Directive::Kind;
        text.ranges.push_back(*directive);
        auto kind = directive_kind(directive_name, {});
        if(kind == If) {
            conditionals.emplace_back(directive->begin, directive->end);
        } else if(!conditionals.empty() && (kind == Else || kind == EndIf)) {
            auto& [begin, branch] = conditionals.back();
            text.loose.push_back(trimmed(content, {branch, directive->begin}));
            branch = directive->end;
            if(kind == EndIf) {
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

        auto open = brackets.visit(token, !skipped.empty() && skipped[i]);
        if(token.is_eod()) {
            if(directive) {
                close_directive();
            }
            continue;
        }
        if(token.is_directive_hash()) {
            directive = token.range;
            directive_name = {};
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
            if(!directive && !brackets.inside() && token.is_identifier() &&
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
        if(open) {
            text.closers.try_emplace(open->begin, token.range.end);
            // `[[ ]]` and `__attribute__(( ))` write one list in two pairs;
            // the inner one, pushed last, is no step of its own.
            const auto* inner = token_after(tokens, open->end);
            // FIXME: a comment between the two closers (`[[a] /* c */ ]`) hides the
            // inner pair: the list is no step, and the declaration's step stops inside it.
            bool doubled = tokens[i - 1].kind == token.kind &&
                           text.closers.lookup(inner->range.begin) == tokens[i - 1].range.end;
            const auto* keyword = token_before(tokens, open->begin);
            auto after = [&](llvm::StringRef word) {
                return keyword && keyword->text(content) == word;
            };
            if(doubled && token.kind == clang::tok::r_square) {
                text.ranges.back() = {open->begin, token.range.end};
                text.attributes.push_back(text.ranges.back());
            } else if(doubled && token.kind == clang::tok::r_paren &&
                      (after("__attribute__") || after("__attribute"))) {
                text.ranges.back() = {keyword->range.begin, token.range.end};
                text.attributes.push_back(text.ranges.back());
            } else {
                text.ranges.push_back(trimmed(content, {open->end, token.range.begin}));
                text.ranges.push_back({open->begin, token.range.end});
                if(token.kind == clang::tok::r_paren && after("__declspec")) {
                    text.ranges.push_back({keyword->range.begin, token.range.end});
                    text.attributes.push_back(text.ranges.back());
                }
            }
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
    auto focus = unit.focus_file();
    auto usable = [&](std::uint32_t i) {
        return !should_ignore_token(focus.kind(tokens[i])) && !semantics.token_preprocessed_away(i);
    };
    auto word = [&](std::uint32_t i) {
        return clang::tok::getPunctuatorSpelling(focus.kind(tokens[i])) == nullptr;
    };

    auto count = static_cast<std::uint32_t>(tokens.size());
    auto indices = std::views::iota(0u, count);
    auto first = static_cast<std::uint32_t>(
        std::ranges::partition_point(
            indices,
            [&](std::uint32_t i) { return tokens[i].range.end < offset; }) -
        indices.begin());

    std::optional<std::uint32_t> touched;
    for(auto i = first; i < count && tokens[i].range.begin <= offset; i += 1) {
        if(usable(i) && (!touched || word(i) || !word(*touched))) {
            touched = i;
        }
    }
    if(touched) {
        // The parentheses and commas of a macro invocation stand for no
        // node; its name stands for what the invocation expands to.
        if(semantics.owners(*touched).empty()) {
            const auto& map = focus.tokens();
            auto expansions = map.expansions_overlapping(tokens.slice(*touched, 1));
            if(!expansions.empty() && !expansions.front().expanded.empty()) {
                const auto& name = expansions.front().spelled.front();
                return {name.range};
            }
        }
        return {tokens[*touched].range};
    }

    llvm::SmallVector<LocalSourceRange, 2> sides;
    auto before = first;
    while(before > 0 && !usable(before - 1)) {
        before -= 1;
    }
    if(before > 0) {
        sides.push_back(tokens[before - 1].range);
    }
    auto after = first;
    while(after < count && !usable(after)) {
        after += 1;
    }
    if(after < count) {
        sides.push_back(tokens[after].range);
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

/// Where an attribute of the declaration at `decl` is written: with its
/// argument list, and the attribute list it is in.
std::optional<LocalSourceRange> attribute_extent(CompilationUnit& unit,
                                                 llvm::ArrayRef<Token> tokens,
                                                 const TextSteps& text,
                                                 const clang::Attr& attr,
                                                 LocalSourceRange decl) {
    auto range = focus_range(unit, attr.getRange());
    if(!range) {
        return std::nullopt;
    }
    // Of the lists ending after the attribute, the first holding it is the
    // innermost (an attribute in a lambda in another's argument).
    auto list = std::ranges::find_if(
        std::ranges::lower_bound(text.attributes, range->end, {}, &LocalSourceRange::end),
        text.attributes.end(),
        [&](const LocalSourceRange& list) { return list.contains(*range); });
    if(list != text.attributes.end()) {
        return *list;
    }
    // A macro argument naming the attribute the macro writes the list of
    // (`#define CXX(x) [[x]]`) is written as the whole invocation, unless
    // the declaration is in it too.
    if(auto invocation = unit.focus_file().produced_range(attr.getRange());
       invocation && !invocation->contains(decl)) {
        *range = hull(*range, *invocation);
    }
    return attribute_range(tokens, text, *range);
}

/// `range`, a declaration's, over the attributes written on it.
LocalSourceRange with_attributes(CompilationUnit& unit,
                                 llvm::ArrayRef<Token> tokens,
                                 const TextSteps& text,
                                 const clang::Decl& decl,
                                 LocalSourceRange range) {
    for(const auto* attr: decl.attrs()) {
        auto written = attr->getRange();
        if(attr->isImplicit() || attr->isInherited() || written.isInvalid() ||
           decls::applied_by_pragma(decl.getASTContext(), written.getBegin())) {
            continue;
        }
        if(auto extent = attribute_extent(unit, tokens, text, *attr, range)) {
            range = hull(range, *extent);
        }
    }
    return range;
}

/// A node's step: a type's with its qualifier, a declaration's with the
/// attributes written on it, an attribute's with its argument list.
std::optional<LocalSourceRange> node_range(CompilationUnit& unit,
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
        return with_attributes(unit, tokens, text, *decl, *range);
    }
    if(node.get<clang::Attr>()) {
        return attribute_range(tokens, text, *range);
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
    return std::ranges::contains(bodies, statement);
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
        if(const auto* parameters = decls::own_template_parameters(decl)) {
            add_parameters(parameters);
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

/// The last of the declarators sharing `decl`'s declaration
/// (`int a = 1, b = 2`).
const clang::Decl& last_declarator(const clang::Decl& decl) {
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
    return *last;
}

/// A function's signature — from its return type, or from the name a
/// constructor's parameters follow, through the parameters, the rest of
/// its type, `override`, `final` and a trailing `requires` clause — and a
/// constructor's initializer list.
void add_function(CompilationUnit& unit,
                  llvm::StringRef content,
                  llvm::ArrayRef<Token> tokens,
                  const clang::FunctionDecl& function,
                  Steps& steps) {
    if(auto type = function.getFunctionTypeLoc()) {
        auto name = function.getQualifierLoc() ? function.getQualifierLoc().getBeginLoc()
                                               : function.getNameInfo().getBeginLoc();
        auto written = focus_range(unit, type.getSourceRange());
        auto named = focus_range(unit, name);
        if(written && named) {
            auto begin = std::min(written->begin, named->begin);
            // A type's location leaves out the qualifiers written before it
            // (`const std::string& name()` starts at `std`).
            for(const auto* token = token_before(tokens, begin);
                token &&
                std::ranges::contains(std::array{"const", "volatile"}, token->text(content));
                token = token_before(tokens, begin)) {
                begin = token->range.begin;
            }
            auto end = written->end;
            auto extend = [&](clang::SourceRange range) {
                if(auto after = focus_range(unit, range)) {
                    end = std::max(end, after->end);
                }
            };
            if(const auto* constraint = function.getTrailingRequiresClause().ConstraintExpr) {
                extend(constraint->getSourceRange());
            }
            for(const auto* attr: function.attrs()) {
                if(llvm::isa<clang::OverrideAttr, clang::FinalAttr>(attr) && !attr->isInherited()) {
                    extend(attr->getRange());
                }
            }
            steps.add({begin, end});
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

/// Whether a statement of a switch's body is a `case` or `default` label,
/// also with attributes (`[[likely]] case 1:`).
bool is_label(const clang::Stmt* statement) {
    if(const auto* attributed = llvm::dyn_cast<clang::AttributedStmt>(statement)) {
        statement = attributed->getSubStmt();
    }
    return llvm::isa<clang::SwitchCase>(statement);
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
    auto first =
        static_cast<std::size_t>(std::ranges::find(statements, statement) - statements.begin());
    while(first > 0 && !is_label(statements[first])) {
        first -= 1;
    }
    if(!is_label(statements[first])) {
        return std::nullopt;
    }
    auto last = first;
    while(last + 1 < statements.size() && !is_label(statements[last + 1])) {
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
        // FIXME: every ancestor is mapped though nest() keeps at most step_limit:
        // in a generated expression 20000 deep each offset costs a third of a second.
        for(const auto* node = tree.common_ancestor(); node && node->parent; node = node->parent) {
            auto range = node_range(unit, tokens, text, *node);
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
            if(const auto& last = last_declarator(*decl); &last != decl) {
                if(auto group = focus_range(unit, last.getSourceRange())) {
                    extent = with_semicolon(
                        hull(*range, with_attributes(unit, tokens, text, last, *group)));
                }
            }
            if(const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                add_function(unit, content, tokens, *function, steps);
            }
            if(node->parent->get<clang::DeclStmt>()) {
                if(auto whole = focus_range(unit, node->parent->source_range())) {
                    extent = hull(extent, *whole);
                }
            }
            if(auto comments = doc_comment_range(unit, decl);
               comments && comments->first == focus) {
                steps.add_loose(hull(extent, comments->second));
            }
        }
    }
}

}  // namespace

auto selection_ranges(CompilationUnit& unit, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto focus = unit.focus_file();
    auto content = focus.content();
    llvm::ArrayRef<Token> tokens = focus.lexical().tokens;
    auto text = scan_text(content, tokens, focus.tokens().preprocessed_away());
    std::vector<std::vector<LocalSourceRange>> result;
    for(auto offset: offsets) {
        Steps steps{.offset = offset};
        add_text(content, tokens, text, steps);
        add_ast(unit, content, tokens, text, steps);
        result.push_back(nest(std::move(steps)));
    }
    return result;
}

auto lexical_selection_ranges(SourceText& source, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>> {
    auto content = source.text();
    llvm::ArrayRef<Token> tokens = source.lexical().tokens;
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
    for(const auto& range: std::views::reverse(ranges)) {
        auto node = std::make_unique<protocol::SelectionRange>();
        node->range = *map.range(range, encoding);
        node->parent = std::move(parent);
        parent = std::move(node);
    }
    return std::move(*parent);
}

}  // namespace clice::feature
