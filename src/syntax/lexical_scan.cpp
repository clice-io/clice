module;

#include "support/prelude.h"

module clice;

import :support.text;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice {

namespace {

using Directive = LexicalInfo::Directive;

LexicalInfo::Comment comment_of(llvm::StringRef content, const Token& token) {
    using Kind = LexicalInfo::Comment::Kind;
    return {token.text(content).starts_with("//") ? Kind::Line : Kind::Block, token.range};
}

/// The macro an include guard's opening tests: `#ifndef X`, `#if !defined X`
/// or `#if !defined(X)`.
std::optional<llvm::StringRef> guard_tested(llvm::StringRef content, llvm::ArrayRef<Token> words) {
    auto spelled = [&](std::size_t index, llvm::StringRef text) {
        return words[index].text(content) == text;
    };
    if(words.size() == 2 && spelled(0, "ifndef") && words[1].is_identifier()) {
        return words[1].text(content);
    }
    if(words.size() < 4 || !spelled(0, "if") || !spelled(1, "!") || !spelled(2, "defined")) {
        return std::nullopt;
    }
    if(words.size() == 4 && words[3].is_identifier()) {
        return words[3].text(content);
    }
    if(words.size() == 6 && spelled(3, "(") && words[4].is_identifier() && spelled(5, ")")) {
        return words[4].text(content);
    }
    return std::nullopt;
}

/// Whether a token of `info` in [from, to) is code: neither a comment nor a
/// directive's.
bool code_in(const LexicalInfo& info, std::uint32_t from, std::uint32_t to) {
    auto& directives = info.directives;
    auto directive = ranges::lower_bound(directives, from, {}, &Directive::first);
    for(auto index = from; index < to; index += 1) {
        if(directive != directives.end() && index == directive->first) {
            index = directive->last;
            ++directive;
        } else if(info.tokens[index].kind != clang::tok::comment) {
            return true;
        }
    }
    return false;
}

std::optional<LexicalInfo::IncludeGuard> find_include_guard(llvm::StringRef content,
                                                            const LexicalInfo& info) {
    auto opening = guard_opening(content, info);
    if(!opening) {
        return std::nullopt;
    }
    auto& directives = info.directives;

    // The `#endif` closing the opening, nothing but `#pragma once` after.
    std::uint32_t closing = LexicalInfo::none;
    for(auto index = *opening + 2; index < directives.size(); index += 1) {
        auto& directive = directives[index];
        if(closing != LexicalInfo::none) {
            if(directive.kind != Directive::Kind::PragmaOnce) {
                return std::nullopt;
            }
        } else if(directive.opening == *opening) {
            closing = index;
        }
    }
    if(closing == LexicalInfo::none || code_in(info,
                                               directives[closing].last + 1,
                                               static_cast<std::uint32_t>(info.tokens.size()))) {
        return std::nullopt;
    }
    return LexicalInfo::IncludeGuard{
        .opening = *opening,
        .definition = *opening + 1,
        .closing = closing,
        .name = info.words(directives[*opening + 1])[1].range,
    };
}

TokenKind closer(TokenKind opener) {
    switch(opener) {
        case clang::tok::l_paren: return clang::tok::r_paren;
        case clang::tok::l_square: return clang::tok::r_square;
        case clang::tok::l_brace: return clang::tok::r_brace;
        default: return clang::tok::unknown;
    }
}

}  // namespace

std::optional<std::uint32_t> guard_opening(llvm::StringRef content, const LexicalInfo& info) {
    auto& directives = info.directives;
    auto opening = static_cast<std::uint32_t>(
        ranges::find_if(directives,
                        [](const Directive& directive) {
                            return directive.kind != Directive::Kind::PragmaOnce;
                        }) -
        directives.begin());
    if(opening + 1 >= directives.size() || directives[opening].kind != Directive::Kind::If) {
        return std::nullopt;
    }
    auto tested = guard_tested(content, info.words(directives[opening]));
    auto& definition = directives[opening + 1];
    if(!tested || definition.kind != Directive::Kind::Define) {
        return std::nullopt;
    }
    auto defined = info.words(definition);
    if(defined.size() < 2 || !defined[1].is_identifier() || defined[1].text(content) != *tested) {
        return std::nullopt;
    }
    if(code_in(info, 0, directives[opening].first) ||
       code_in(info, directives[opening].last + 1, definition.first)) {
        return std::nullopt;
    }
    if(ranges::any_of(llvm::ArrayRef(directives).drop_front(opening + 2),
                      [&](const Directive& directive) {
                          return directive.opening == opening &&
                                 directive.kind == Directive::Kind::Else;
                      })) {
        return std::nullopt;
    }
    return opening;
}

Directive::Kind directive_kind(llvm::StringRef name, llvm::StringRef argument) {
    using enum Directive::Kind;
    if(name == "if" || name == "ifdef" || name == "ifndef") {
        return If;
    }
    if(name == "elif" || name == "elifdef" || name == "elifndef" || name == "else") {
        return Else;
    }
    if(name == "endif") {
        return EndIf;
    }
    if(name == "include" || name == "include_next" || name == "import") {
        return Include;
    }
    if(name == "define") {
        return Define;
    }
    if(name != "pragma") {
        return Other;
    }
    // Only the first argument decides: `#pragma mark endregion` closes
    // nothing.
    if(argument == "region") {
        return Region;
    }
    if(argument == "endregion") {
        return EndRegion;
    }
    if(argument == "mark") {
        return PragmaMark;
    }
    return argument == "once" ? PragmaOnce : Other;
}

std::optional<MacroDefinition> macro_definition(llvm::ArrayRef<Token> words) {
    if(words.size() < 2 || !words[1].is_identifier()) {
        return std::nullopt;
    }
    MacroDefinition definition{.name = words[1], .body = words.drop_front(2)};
    // Function-like when the parenthesis touches the name.
    if(auto& body = definition.body; !body.empty() && body.front().kind == clang::tok::l_paren &&
                                     body.front().range.begin == definition.name.range.end) {
        auto close = ranges::find_if(body, [](const Token& token) {
            return token.kind == clang::tok::r_paren;
        });
        auto length = close == body.end() ? body.size() : close - body.begin() + 1;
        definition.parameters = body.take_front(length);
        body = body.drop_front(length);
    }
    return definition;
}

LexicalInfo lexical_scan(llvm::StringRef content, const clang::LangOptions& dialect) {
    using ModuleDeclaration = LexicalInfo::ModuleDeclaration;

    LexicalInfo info;
    Lexer lexer(content, dialect, {.keep_comments = true});

    auto record = [&](const Token& token) {
        if(token.is_eof()) {
            return;
        }
        info.tokens.push_back(token);
        if(token.kind == clang::tok::comment) {
            info.comments.push_back(comment_of(content, token));
        }
    };
    // The next token but a comment; the comments before it are recorded.
    auto lex = [&] {
        while(true) {
            auto token = lexer.advance();
            record(token);
            if(token.kind != clang::tok::comment) {
                return token;
            }
        }
    };
    // What lex() returns next, unlexed but for the comments before it.
    auto peek = [&] {
        while(lexer.next().kind == clang::tok::comment) {
            record(lexer.advance());
        }
        return lexer.next();
    };

    auto is_spelled = [&](const Token& token, llvm::StringRef spelling) {
        return token.is_identifier() && token.text(content) == spelling;
    };

    // Open conditional blocks, innermost last, by their `#if`.
    llvm::SmallVector<std::uint32_t> open;

    auto scan_directive = [&](const Token& hash) {
        Directive directive{.first = static_cast<std::uint32_t>(info.tokens.size() - 1)};
        auto name = lex();
        auto argument = name;
        if(!name.is_eod() && !name.is_eof()) {
            argument = lex();
        }
        auto token = argument;
        while(!token.is_eod() && !token.is_eof()) {
            token = lex();
        }
        directive.last = static_cast<std::uint32_t>(info.tokens.size() - 1);
        directive.range = {hash.range.begin, token.range.begin};
        auto word = [&](const Token& part) {
            return part.is_eod() || part.is_eof() ? llvm::StringRef() : part.text(content);
        };
        directive.kind = directive_kind(word(name), word(argument));

        auto index = static_cast<std::uint32_t>(info.directives.size());
        directive.depth = static_cast<std::uint32_t>(open.size());
        if(directive.kind == Directive::Kind::If) {
            open.push_back(index);
        } else if((directive.kind == Directive::Kind::Else ||
                   directive.kind == Directive::Kind::EndIf) &&
                  !open.empty()) {
            directive.opening = open.back();
            directive.depth -= 1;
            if(directive.kind == Directive::Kind::EndIf) {
                open.pop_back();
            }
        }
        info.directives.push_back(directive);
    };

    auto advance_if = [&](llvm::function_ref<bool(const Token&)> pred) -> std::optional<Token> {
        if(auto token = peek(); pred(token)) {
            return lex();
        }
        return std::nullopt;
    };

    auto advance_if_kind = [&](TokenKind kind) {
        return advance_if([&](const Token& token) { return token.kind == kind; });
    };

    auto lex_dotted = [&](llvm::SmallVectorImpl<LocalSourceRange>& parts) {
        while(true) {
            auto part = advance_if([](const Token& token) { return token.is_identifier(); });
            if(!part) {
                return;
            }
            parts.push_back(part->range);
            if(!advance_if_kind(clang::tok::period)) {
                return;
            }
        }
    };

    bool file_start = true;

    while(true) {
        auto token = lex();
        if(token.is_eof()) {
            break;
        }

        bool at_file_start = file_start;
        file_start = false;

        if(token.is_directive_hash()) {
            scan_directive(token);
            continue;
        }

        if(token.is_raw_string(content)) {
            info.raw_strings.push_back(token.range);
            continue;
        }

        // Valid code cannot begin a logical line with `module` (or `export
        // module`) in any other meaning, so line-start matching is exact;
        // stray matches in invalid code are filtered by the consumers'
        // compiler-state cross-checks.
        if(!token.is_at_start_of_line || !token.is_identifier()) {
            continue;
        }

        ModuleDeclaration decl;
        if(is_spelled(token, "export")) {
            if(!is_spelled(peek(), "module")) {
                continue;
            }
            decl.export_keyword = token.range;
            token = lex();
        } else if(!is_spelled(token, "module")) {
            continue;
        }
        decl.keyword = token.range;

        // `module;` introduces the global module fragment only as the
        // file's first token; a bare `module;` anywhere else is nothing.
        if(peek().kind == clang::tok::semi) {
            if(at_file_start && !decl.export_keyword.valid()) {
                decl.kind = ModuleDeclaration::Kind::GlobalFragment;
                info.modules.push_back(std::move(decl));
            }
            continue;
        }

        // `module :private;` — the private fragment takes no export.
        if(auto colon = advance_if_kind(clang::tok::colon)) {
            decl.colon = colon->range;
            auto is_private = [&](const Token& token) {
                return is_spelled(token, "private");
            };
            if(auto private_keyword = advance_if(is_private);
               private_keyword && !decl.export_keyword.valid()) {
                decl.kind = ModuleDeclaration::Kind::PrivateFragment;
                decl.partition_parts.push_back(private_keyword->range);
                info.modules.push_back(std::move(decl));
            }
            continue;
        }

        lex_dotted(decl.name_parts);
        if(decl.name_parts.empty()) {
            continue;
        }

        if(auto colon = advance_if_kind(clang::tok::colon)) {
            decl.colon = colon->range;
            lex_dotted(decl.partition_parts);
        }

        decl.kind = ModuleDeclaration::Kind::Declaration;
        info.modules.push_back(std::move(decl));
    }

    info.include_guard = find_include_guard(content, info);
    return info;
}

std::vector<LexicalInfo::Comment> scan_comments(llvm::StringRef content,
                                                const clang::LangOptions& dialect) {
    std::vector<LexicalInfo::Comment> comments;
    Lexer lexer(content, dialect, {.keep_comments = true});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(token.kind == clang::tok::comment) {
            comments.push_back(comment_of(content, token));
        }
    }
    return comments;
}

std::optional<std::size_t> code_before(llvm::ArrayRef<Token> tokens, std::size_t index) {
    while(index > 0) {
        index -= 1;
        if(tokens[index].kind != clang::tok::comment) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> code_after(llvm::ArrayRef<Token> tokens, std::size_t index) {
    for(index += 1; index < tokens.size(); index += 1) {
        if(tokens[index].kind != clang::tok::comment) {
            return index;
        }
    }
    return std::nullopt;
}

llvm::SmallVector<Token, 8> LexicalInfo::words(const Directive& directive) const {
    llvm::SmallVector<Token, 8> words;
    for(auto& token:
        llvm::ArrayRef(tokens).slice(directive.first + 1, directive.last - directive.first - 1)) {
        if(token.kind != clang::tok::comment) {
            words.push_back(token);
        }
    }
    return words;
}

std::uint32_t LexicalInfo::token_at(std::uint32_t offset) const {
    return static_cast<std::uint32_t>(
        ranges::upper_bound(tokens,
                            offset,
                            {},
                            [](const Token& token) { return token.range.end; }) -
        tokens.begin());
}

auto LexicalInfo::directive_at(llvm::StringRef content, std::uint32_t offset) const
    -> const Directive* {
    auto it = ranges::lower_bound(directives, offset, {}, [](const Directive& directive) {
        return directive.range.end;
    });
    if(it == directives.end() || line_begin(content, it->range.begin) > offset) {
        return nullptr;
    }
    return &*it;
}

std::optional<LocalSourceRange> find_directive_argument(const LexicalInfo& info,
                                                        llvm::StringRef content,
                                                        std::uint32_t offset) {
    const auto* directive = info.directive_at(content, offset);
    if(!directive) {
        return std::nullopt;
    }
    auto words = info.words(*directive);
    bool after_keyword = false;
    for(std::size_t index = 0; index < words.size(); index += 1) {
        auto& token = words[index];

        // A filename passed through a macro argument (`#if HAS(<c.h>)`)
        // follows no keyword of its own: the offset pins its start.
        if(token.range.begin == offset) {
            if(token.kind == clang::tok::string_literal) {
                return token.range;
            }
            if(token.kind == clang::tok::less) {
                for(auto& close: llvm::ArrayRef(words).drop_front(index + 1)) {
                    if(close.kind == clang::tok::greater) {
                        return LocalSourceRange{token.range.begin, close.range.end};
                    }
                }
                return std::nullopt;
            }
        }

        if(token.is_identifier()) {
            auto text = token.text(content);
            // The __has_include family are reserved operators that may recur
            // within one #if line; every occurrence restarts the match.
            if(text == "__has_include" || text == "__has_include_next" || text == "__has_embed") {
                after_keyword = true;
                continue;
            }
            // A directive keyword only counts before the first match; a
            // later one is a macro standing in for the filename (legal
            // pre-C++20 even for one literally named `import`).
            if(!after_keyword && takes_header_name(text)) {
                after_keyword = true;
                continue;
            }
        }

        if(token.range.end <= offset || !after_keyword) {
            continue;
        }

        if(token.is_header_name()) {
            // A spliced directive's header name starts at its splice.
            auto skipped = token.text(content).find_first_of("<\"");
            return LocalSourceRange{token.range.begin + static_cast<std::uint32_t>(skipped),
                                    token.range.end};
        }
        if(token.kind == clang::tok::string_literal || token.is_identifier()) {
            return token.range;
        }
    }
    return std::nullopt;
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
    // FIXME: skipped code is one scope however many conditionals it nests, so
    // a brace an inner `#if`/`#else` splits pairs across the branches (and the
    // branch's own fold loses to that pair).
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
    if(!ranges::contains(std::array{clang::tok::r_paren, clang::tok::r_square, clang::tok::r_brace},
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

std::vector<Brace> code_braces(const LexicalInfo& lexical) {
    std::vector<Brace> braces;
    auto directive = lexical.directives.begin();
    for(std::uint32_t index = 0; index < lexical.tokens.size(); index += 1) {
        if(directive != lexical.directives.end() && index == directive->first) {
            index = directive->last;
            ++directive;
            continue;
        }
        const auto& token = lexical.tokens[index];
        if(token.kind == clang::tok::l_brace || token.kind == clang::tok::r_brace) {
            braces.push_back({token.range, token.kind == clang::tok::l_brace});
        }
    }
    return braces;
}

std::optional<LocalSourceRange> last_brace_group(llvm::ArrayRef<Brace> braces,
                                                 LocalSourceRange extent) {
    auto begin =
        ranges::lower_bound(braces, extent.begin, {}, [](const Brace& b) { return b.range.begin; });
    std::optional<LocalSourceRange> group;
    std::int32_t depth = 0;
    std::uint32_t open_begin = 0;
    for(auto it = begin; it != braces.end() && it->range.end <= extent.end; ++it) {
        if(it->open) {
            if(depth == 0) {
                open_begin = it->range.begin;
            }
            depth += 1;
        } else if(depth > 0) {
            depth -= 1;
            if(depth == 0) {
                group = LocalSourceRange{open_begin, it->range.end};
            }
        }
    }
    return group;
}

}  // namespace clice
