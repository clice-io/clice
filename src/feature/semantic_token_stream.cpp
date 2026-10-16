module;

#include "modules/prelude.h"

module clice;

import :feature.feature;
import :feature.semantic_token_stream;
import :semantic.token_class;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.lexer;

namespace clice::feature {

namespace {

using Kind = SemanticTokenKind;
using Modifier = SemanticTokenModifier;

/// Whether a punctuator is an operator: everything but brackets,
/// separators, member access and the preprocessor's own.
bool is_operator(clang::tok::TokenKind kind) {
    switch(kind) {
        case clang::tok::amp:
        case clang::tok::ampamp:
        case clang::tok::ampequal:
        case clang::tok::star:
        case clang::tok::starequal:
        case clang::tok::plus:
        case clang::tok::plusplus:
        case clang::tok::plusequal:
        case clang::tok::minus:
        case clang::tok::minusminus:
        case clang::tok::minusequal:
        case clang::tok::tilde:
        case clang::tok::exclaim:
        case clang::tok::exclaimequal:
        case clang::tok::slash:
        case clang::tok::slashequal:
        case clang::tok::percent:
        case clang::tok::percentequal:
        case clang::tok::less:
        case clang::tok::lessless:
        case clang::tok::lessequal:
        case clang::tok::lesslessequal:
        case clang::tok::spaceship:
        case clang::tok::greater:
        case clang::tok::greatergreater:
        case clang::tok::greaterequal:
        case clang::tok::greatergreaterequal:
        case clang::tok::caret:
        case clang::tok::caretequal:
        case clang::tok::pipe:
        case clang::tok::pipepipe:
        case clang::tok::pipeequal:
        case clang::tok::question:
        case clang::tok::equal:
        case clang::tok::equalequal:
        case clang::tok::periodstar:
        case clang::tok::arrowstar: return true;
        default: return false;
    }
}

bool is_string_literal(clang::tok::TokenKind kind) {
    return clang::tok::isStringLiteral(kind);
}

bool is_char_literal(clang::tok::TokenKind kind) {
    return kind == clang::tok::char_constant || kind == clang::tok::wide_char_constant ||
           kind == clang::tok::utf8_char_constant || kind == clang::tok::utf16_char_constant ||
           kind == clang::tok::utf32_char_constant;
}

bool is_hex_digit(char c) {
    return llvm::isHexDigit(c);
}

/// The length of the escape sequence at `text[0]`, a backslash.
std::size_t escape_length(llvm::StringRef text) {
    if(text.size() < 2) {
        return text.size();
    }
    auto braced = [&](std::size_t from) {
        auto close = text.find('}', from);
        return close == llvm::StringRef::npos ? text.size() : close + 1;
    };
    auto run = [&](std::size_t from, std::size_t limit, auto&& accept) {
        auto end = from;
        while(end < text.size() && end - from < limit && accept(text[end])) {
            end += 1;
        }
        return end;
    };
    switch(text[1]) {
        case 'x': {
            if(text.size() > 2 && text[2] == '{') {
                return braced(3);
            }
            return run(2, text.size(), is_hex_digit);
        }
        case 'u':
        case 'U': {
            if(text.size() > 2 && text[2] == '{') {
                return braced(3);
            }
            return run(2, text[1] == 'u' ? 4 : 8, is_hex_digit);
        }
        case 'N':
        case 'o': {
            if(text.size() > 2 && text[2] == '{') {
                return braced(3);
            }
            return 2;
        }
        default: {
            if(text[1] >= '0' && text[1] <= '7') {
                return run(1, 3, [](char c) { return c >= '0' && c <= '7'; });
            }
            return 2;
        }
    }
}

/// The length of the std::format replacement field at `text[0]`, a `{`
/// not doubled, through its `}`; one level of nested fields (`{:{}}`) is
/// allowed. Zero for a field left open.
std::size_t replacement_field_length(llvm::StringRef text) {
    std::uint32_t depth = 0;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        if(text[i] == '{') {
            depth += 1;
        } else if(text[i] == '}') {
            depth -= 1;
            if(depth == 0) {
                return i + 1;
            }
        } else if(text[i] == '\\' || text[i] == '"') {
            return 0;
        }
    }
    return 0;
}

/// The length of the printf conversion specification at `text[0]`, a `%`:
/// flags, width, precision, length and conversion. Zero for none.
std::size_t conversion_length(llvm::StringRef text) {
    std::size_t i = 1;
    auto skip = [&](llvm::StringRef set) {
        while(i < text.size() && set.contains(text[i])) {
            i += 1;
        }
    };
    skip("-+ #0'");
    if(i < text.size() && text[i] == '*') {
        i += 1;
    } else {
        skip("0123456789");
    }
    if(i < text.size() && text[i] == '.') {
        i += 1;
        if(i < text.size() && text[i] == '*') {
            i += 1;
        } else {
            skip("0123456789");
        }
    }
    skip("hljztLqI0123456789");
    if(i < text.size() && llvm::StringRef("diouxXeEfFgGaAcspnCSm").contains(text[i])) {
        return i + 1;
    }
    return 0;
}

/// Where the suffix of a numeric literal starts: past its digits,
/// separators, radix point and exponent.
std::size_t number_suffix(llvm::StringRef text) {
    std::size_t i = 0;
    bool hex = text.starts_with_insensitive("0x");
    bool binary = text.starts_with_insensitive("0b");
    if(hex || binary) {
        i = 2;
    }
    auto digit = [&](char c) {
        return hex ? is_hex_digit(c) : binary ? (c == '0' || c == '1') : llvm::isDigit(c);
    };
    while(i < text.size()) {
        char c = text[i];
        if(digit(c) || c == '.' || (c == '\'' && i + 1 < text.size() && digit(text[i + 1]))) {
            i += 1;
        } else if((hex ? (c == 'p' || c == 'P') : (!binary && (c == 'e' || c == 'E'))) &&
                  i + 1 < text.size() &&
                  (llvm::isDigit(text[i + 1]) || text[i + 1] == '+' || text[i + 1] == '-')) {
            i += 2;
            while(i < text.size() && llvm::isDigit(text[i])) {
                i += 1;
            }
        } else {
            break;
        }
    }
    return i;
}

class TokenStream {
public:
    TokenStream(llvm::StringRef content,
                const clang::LangOptions& lang_opts,
                const TokenFacts& facts,
                const SemanticTokensOptions& options) :
        content(content), lang_opts(lang_opts), keywords(lang_opts), facts(facts),
        options(options) {}

    auto build() -> std::vector<SemanticToken> {
        Lexer lexer(content, {.keep_comments = true, .lang_opts = &lang_opts});
        for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
            if(token.is_eod()) {
                end_directive();
                continue;
            }
            if(token.kind == clang::tok::comment) {
                comment(token.range);
                continue;
            }
            classify(lexer, token);
        }
        return std::move(tokens);
    }

private:
    enum class Directive : std::uint8_t {
        None,
        /// After the `#`, before the directive's name.
        AfterHash,
        /// Before the name of a `#define`.
        AfterDefine,
        /// In the parameter list of a `#define`.
        Parameters,
        /// In the replacement list of a `#define`.
        Body,
        /// Before the macro name `#ifdef`, `#ifndef`, `#undef` take.
        MacroName,
        /// In the condition of an `#if` or `#elif`.
        Condition,
        /// After `defined` in a condition.
        Defined,
        /// Anywhere else in a directive.
        Other,
    };

    struct Piece {
        TokenClass classified;
        /// Sent with lexical tokens off too: what the editor's syntax
        /// highlighting cannot tell.
        bool semantic;
    };

    void classify(Lexer& lexer, const Token& token) {
        if(directive == Directive::Body) {
            definition_empty = false;
        }
        auto spelling = token.text(content);
        auto kind = token.kind;
        if(token.is_identifier()) {
            auto it = keywords.find(spelling);
            kind = it == keywords.end() ? clang::tok::identifier : it->second->getTokenID();
        }
        bool word = clang::tok::isAnyIdentifier(kind) || clang::tok::getKeywordSpelling(kind);

        if(is_string_literal(kind) || is_char_literal(kind)) {
            literal(token.range, kind);
            after(kind, false);
            return;
        }
        if(kind == clang::tok::numeric_constant) {
            number(token.range);
            after(kind, false);
            return;
        }

        if(kind == clang::tok::greatergreater && closes_bracket()) {
            // A `>>` closing a template's arguments, and the list around
            // them or a shift.
            auto middle = token.range.begin + 1;
            brackets.pop_back();
            emit({token.range.begin, middle}, {{.kind = Kind::Bracket}, true});
            bool closes = closes_bracket();
            if(closes) {
                brackets.pop_back();
            }
            emit({middle, token.range.end}, {{.kind = closes ? Kind::Bracket : Kind::Operator}, closes});
            after(kind, false);
            return;
        }

        auto piece = lexical(lexer, token, kind, spelling, word);
        if(auto semantic = semantic_of(token, spelling, word)) {
            piece = {*semantic, true};
        } else if(word && attribute && depth == attribute->depth) {
            piece = {{.kind = Kind::Attribute}, true};
        }
        piece = bracket(kind, piece);
        follow_attribute(kind);
        emit(token.range, piece);
        if(piece.classified.kind == Kind::Macro && piece.classified.has(Modifier::Definition) &&
           !tokens.empty() && tokens.back().range.begin == token.range.begin) {
            definition = tokens.size() - 1;
        }
        after(kind, piece.classified.has(Modifier::Template) || piece.classified.kind == Kind::Concept);
    }

    /// Ends a directive: a `#define`'s name learns what its replacement
    /// list holds.
    void end_directive() {
        if(definition) {
            auto& name = tokens[*definition];
            if(definition_empty) {
                name.modifiers |= mask(Modifier::Empty);
            }
            if(definition_control) {
                name.modifiers |= mask(Modifier::ControlFlow);
            }
        }
        definition.reset();
        definition_empty = true;
        definition_control = false;
        directive = Directive::None;
        macro_parameters.clear();
    }

    /// Where attribute names are written: in `[[...]]`, in the double
    /// parentheses of `__attribute__`, in those of `__declspec`; their
    /// arguments sit deeper.
    void follow_attribute(clang::tok::TokenKind kind) {
        if(attribute) {
            if(depth >= attribute->depth) {
                attribute->entered = true;
            } else if(attribute->entered) {
                attribute.reset();
            }
            return;
        }
        if(kind == clang::tok::l_square && previous == clang::tok::l_square) {
            attribute = {.depth = depth, .entered = true};
        } else if(kind == clang::tok::kw___attribute) {
            attribute = {.depth = depth + 2};
        } else if(kind == clang::tok::kw___declspec) {
            attribute = {.depth = depth + 1};
        }
    }

    bool closes_bracket() const {
        return !brackets.empty() && brackets.back() == depth;
    }

    /// What a token's spelling says it is, the directive it sits in
    /// included.
    Piece lexical(Lexer& lexer, const Token& token, clang::tok::TokenKind kind,
                  llvm::StringRef spelling, bool word) {
        Piece piece{.semantic = false};
        if(clang::tok::getKeywordSpelling(kind)) {
            piece.classified = keyword_class(kind);
            if(kind == clang::tok::kw_default && lexer.next().kind == clang::tok::colon) {
                piece.classified.modifiers |= mask(Modifier::ControlFlow);
            }
        } else if(clang::tok::getPunctuatorSpelling(kind)) {
            // Alternative spellings (`and`, `not`) lex as their punctuators.
            if(is_operator(kind) || (!spelling.empty() && llvm::isAlpha(spelling.front()))) {
                piece.classified.kind = Kind::Operator;
            }
        } else if(token.is_header_name()) {
            piece.classified.kind = Kind::Header;
        }

        // `module` and `import` are keywords where a line starts with them
        // (behind `export` at most) before what a module declaration or an
        // import takes; `override` and `final` after a declarator.
        bool line_head = token.is_at_start_of_line || after_line_export;
        after_line_export = token.is_at_start_of_line && spelling == "export";
        if(kind == clang::tok::identifier && directive == Directive::None) {
            if(lang_opts.CPlusPlusModules && line_head && (spelling == "module" || spelling == "import")) {
                auto next = lexer.next();
                bool opens =
                    spelling == "module"
                        ? llvm::is_contained(
                              {clang::tok::raw_identifier, clang::tok::colon, clang::tok::semi},
                              next.kind)
                        : llvm::is_contained({clang::tok::raw_identifier,
                                              clang::tok::colon,
                                              clang::tok::less,
                                              clang::tok::header_name,
                                              clang::tok::string_literal},
                                             next.kind);
                if(opens) {
                    return {{.kind = Kind::Keyword}, true};
                }
            }
            if(lang_opts.CPlusPlus &&
               (spelling == "override" || spelling == "final" || spelling == "sealed")) {
                return {{.kind = Kind::Keyword}, true};
            }
        }

        switch(directive) {
            case Directive::None: {
                if(token.is_directive_hash()) {
                    directive = Directive::AfterHash;
                    piece.classified = {.kind = Kind::Directive};
                }
                break;
            }
            case Directive::AfterHash: {
                if(!word) {
                    directive = Directive::Other;
                    break;
                }
                piece.classified = {.kind = Kind::Directive};
                if(spelling == "define") {
                    directive = Directive::AfterDefine;
                } else if(llvm::is_contained({"ifdef", "ifndef", "elifdef", "elifndef", "undef"},
                                             spelling)) {
                    directive = Directive::MacroName;
                } else if(spelling == "if" || spelling == "elif") {
                    directive = Directive::Condition;
                } else {
                    directive = Directive::Other;
                }
                break;
            }
            case Directive::AfterDefine: {
                if(!word) {
                    directive = Directive::Other;
                    break;
                }
                bool function_like = lexer.next().kind == clang::tok::l_paren &&
                                     lexer.next().range.begin == token.range.end;
                piece = {{.kind = Kind::Macro,
                          .modifiers = mask(Modifier::Definition) |
                                       (function_like ? mask(Modifier::Callable) : 0)},
                         true};
                directive = function_like ? Directive::Parameters : Directive::Body;
                break;
            }
            case Directive::Parameters: {
                if(word) {
                    macro_parameters.push_back(spelling);
                    piece = {{.kind = Kind::MacroParameter}, true};
                } else if(kind == clang::tok::ellipsis) {
                    macro_parameters.push_back("__VA_ARGS__");
                } else if(kind == clang::tok::r_paren) {
                    directive = Directive::Body;
                }
                break;
            }
            case Directive::Body: {
                definition_control |= llvm::is_contained({clang::tok::kw_return,
                                                          clang::tok::kw_break,
                                                          clang::tok::kw_continue,
                                                          clang::tok::kw_goto,
                                                          clang::tok::kw_throw,
                                                          clang::tok::kw_co_return},
                                                         kind);
                if(word && llvm::is_contained(macro_parameters, spelling)) {
                    piece = {{.kind = Kind::MacroParameter}, true};
                }
                break;
            }
            case Directive::MacroName: {
                if(word) {
                    piece = {{.kind = Kind::Macro}, true};
                }
                directive = Directive::Other;
                break;
            }
            case Directive::Condition: {
                if(spelling == "defined") {
                    piece.classified = {.kind = Kind::Operator};
                    directive = Directive::Defined;
                }
                break;
            }
            case Directive::Defined: {
                if(word) {
                    piece = {{.kind = Kind::Macro}, true};
                    directive = Directive::Condition;
                } else if(kind != clang::tok::l_paren) {
                    directive = Directive::Condition;
                }
                break;
            }
            case Directive::Other: break;
        }
        return piece;
    }

    /// What the facts say of a token, a name standing in for a keyword
    /// (`#define bool _Bool`) shown as the keyword.
    std::optional<TokenClass> semantic_of(const Token& token, llvm::StringRef spelling, bool word) {
        if(directive == Directive::AfterDefine || directive == Directive::Parameters ||
           directive == Directive::Body) {
            return std::nullopt;
        }
        auto it = facts.semantic.find(token.range.begin);
        if(it == facts.semantic.end()) {
            return std::nullopt;
        }
        auto classified = it->second;
        if(classified.kind == Kind::Macro && word && directive == Directive::None) {
            if(auto keyword = keyword_of(spelling); keyword != clang::tok::identifier) {
                return keyword_class(keyword);
            }
        }
        return classified;
    }

    /// A `<` opening a template's arguments or parameters and the `>`
    /// closing them, by what precedes the `<`: a template's name, the
    /// `template` keyword, a name after it.
    Piece bracket(clang::tok::TokenKind kind, Piece piece) {
        switch(kind) {
            case clang::tok::l_paren:
            case clang::tok::l_square:
            case clang::tok::l_brace: {
                depth += 1;
                break;
            }
            case clang::tok::r_paren:
            case clang::tok::r_square:
            case clang::tok::r_brace: {
                if(depth > 0) {
                    depth -= 1;
                }
                while(!brackets.empty() && brackets.back() > depth) {
                    brackets.pop_back();
                }
                break;
            }
            case clang::tok::less: {
                if(names_template) {
                    brackets.push_back(depth);
                    return {{.kind = Kind::Bracket}, true};
                }
                break;
            }
            case clang::tok::greater: {
                if(closes_bracket()) {
                    brackets.pop_back();
                    return {{.kind = Kind::Bracket}, true};
                }
                break;
            }
            default: break;
        }
        return piece;
    }

    /// Records what the next `<` follows.
    void after(clang::tok::TokenKind kind, bool template_name) {
        names_template = template_name || (after_template && clang::tok::isAnyIdentifier(kind)) ||
                         kind == clang::tok::kw_template;
        after_template = kind == clang::tok::kw_template;
        previous = kind;
    }

    void literal(LocalSourceRange range, clang::tok::TokenKind kind) {
        auto text = content.slice(range.begin, range.end);
        auto base = is_char_literal(kind) ? Kind::Character : Kind::String;
        char quote = base == Kind::Character ? '\'' : '"';
        auto open = text.find(quote);
        auto close = text.rfind(quote);
        if(open == llvm::StringRef::npos || close == open) {
            emit(range, {{.kind = base}, false});
            return;
        }
        bool raw = open > 0 && text[open - 1] == 'R';

        auto user_defined = [&] {
            auto it = facts.semantic.find(range.begin);
            return it != facts.semantic.end() && it->second.has(Modifier::UserDefined);
        }();
        std::optional<FormatStyle> format;
        if(auto it = facts.formats.find(range.begin); it != facts.formats.end()) {
            format = it->second;
        }

        auto at = [&](std::size_t offset) {
            return range.begin + static_cast<std::uint32_t>(offset);
        };
        if(open > 0) {
            emit({range.begin, at(open)}, {{.kind = base, .modifiers = mask(Modifier::Affix)}, false});
        }
        std::size_t segment = open;
        auto flush = [&](std::size_t until) {
            if(until > segment) {
                emit({at(segment), at(until)}, {{.kind = base}, false});
            }
        };
        for(std::size_t i = open + 1; i < close && !raw;) {
            std::size_t length = 0;
            Piece piece;
            if(text[i] == '\\') {
                length = std::min(escape_length(text.substr(i)), close - i);
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Std && (text[i] == '{' || text[i] == '}') &&
                      i + 1 < close && text[i + 1] == text[i]) {
                length = 2;
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Std && text[i] == '{') {
                length = replacement_field_length(text.slice(i, close));
                piece = {{.kind = Kind::FormatSpecifier}, true};
            } else if(format == FormatStyle::Printf && text[i] == '%' && i + 1 < close &&
                      text[i + 1] == '%') {
                length = 2;
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Printf && text[i] == '%') {
                length = conversion_length(text.slice(i, close));
                piece = {{.kind = Kind::FormatSpecifier}, true};
            }
            if(length == 0) {
                i += 1;
                continue;
            }
            flush(i);
            emit({at(i), at(i + length)}, piece);
            i += length;
            segment = i;
        }
        flush(close + 1);
        if(close + 1 < text.size()) {
            suffix({at(close + 1), range.end}, base, user_defined);
        }
    }

    void number(LocalSourceRange range) {
        auto text = content.slice(range.begin, range.end);
        auto split = number_suffix(text);
        auto user_defined = [&] {
            auto it = facts.semantic.find(range.begin);
            return it != facts.semantic.end() && it->second.has(Modifier::UserDefined);
        }();
        auto at = range.begin + static_cast<std::uint32_t>(split);
        emit({range.begin, at}, {{.kind = Kind::Number}, false});
        if(at < range.end) {
            suffix({at, range.end}, Kind::Number, user_defined);
        }
    }

    void suffix(LocalSourceRange range, Kind base, bool user_defined) {
        TokenClass classified{.kind = base, .modifiers = mask(Modifier::Affix)};
        if(user_defined) {
            classified.modifiers |= mask(Modifier::UserDefined);
        }
        emit(range, {classified, user_defined});
    }

    /// A comment; a documentation comment's commands as keywords and the
    /// names `@param` and `@tparam` document as what they name.
    void comment(LocalSourceRange range) {
        auto text = content.slice(range.begin, range.end);
        if(!is_doc_comment(text)) {
            emit(range, {{.kind = Kind::Comment}, false});
            return;
        }
        auto documentation = mask(Modifier::Documentation);
        auto at = [&](std::size_t offset) {
            return range.begin + static_cast<std::uint32_t>(offset);
        };
        std::size_t segment = 0;
        for(std::size_t i = 3; i < text.size(); i += 1) {
            if((text[i] != '@' && text[i] != '\\') || !llvm::isSpace(text[i - 1]) ||
               i + 1 >= text.size() || !llvm::isAlpha(text[i + 1])) {
                continue;
            }
            auto name_end = i + 1;
            while(name_end < text.size() && llvm::isAlpha(text[name_end])) {
                name_end += 1;
            }
            auto name = text.slice(i + 1, name_end);
            if(!is_doc_command(name)) {
                continue;
            }
            emit({at(segment), at(i)}, {{.kind = Kind::Comment, .modifiers = documentation}, false});
            emit({at(i), at(name_end)}, {{.kind = Kind::Keyword, .modifiers = documentation}, false});
            segment = name_end;
            i = name_end - 1;
            if(name != "param" && name != "tparam") {
                continue;
            }
            // `@param[in] name`, `@param name`.
            auto word = name_end;
            if(word < text.size() && text[word] == '[') {
                auto close = text.find(']', word);
                word = close == llvm::StringRef::npos ? text.size() : close + 1;
            }
            while(word < text.size() && (text[word] == ' ' || text[word] == '\t')) {
                word += 1;
            }
            auto word_end = word;
            while(word_end < text.size() &&
                  (llvm::isAlnum(text[word_end]) || text[word_end] == '_')) {
                word_end += 1;
            }
            if(word_end == word) {
                continue;
            }
            emit({at(segment), at(word)}, {{.kind = Kind::Comment, .modifiers = documentation}, false});
            auto kind = name == "param" ? Kind::Parameter : Kind::TypeParameter;
            emit({at(word), at(word_end)}, {{.kind = kind, .modifiers = documentation}, true});
            segment = word_end;
            i = word_end - 1;
        }
        emit({at(segment), range.end}, {{.kind = Kind::Comment, .modifiers = documentation}, false});
    }

    /// Whether `range` overlaps an inactive span. Emitted ranges ascend, so
    /// one cursor over the sorted disjoint spans suffices.
    bool inactive(LocalSourceRange range) {
        auto& spans = facts.inactive;
        while(next_span + 1 < spans.size() && spans[next_span + 1] <= range.begin) {
            next_span += 2;
        }
        return next_span + 1 < spans.size() && spans[next_span] < range.end;
    }

    /// The single exit of every token, so Inactive and the lexical switch
    /// apply to all of them; a token split in two (`>>`) emits its tail.
    void emit(LocalSourceRange range, Piece piece) {
        if(range.begin >= range.end) {
            return;
        }
        auto classified = piece.classified;
        if(inactive(range)) {
            classified = {.kind = options.lexical && classified.kind != Kind::None ? classified.kind
                                                                                  : Kind::Plain,
                         .modifiers = classified.modifiers | mask(Modifier::Inactive)};
        } else if(classified.kind == Kind::None || (!options.lexical && !piece.semantic)) {
            return;
        }
        if(!tokens.empty()) {
            auto& last = tokens.back();
            if(last.range.end == range.begin && last.kind == classified.kind &&
               last.modifiers == classified.modifiers) {
                last.range.end = range.end;
                return;
            }
        }
        tokens.push_back({.range = range, .kind = classified.kind, .modifiers = classified.modifiers});
    }

    llvm::StringRef content;
    const clang::LangOptions& lang_opts;
    clang::IdentifierTable keywords;
    const TokenFacts& facts;
    const SemanticTokensOptions& options;

    Directive directive = Directive::None;
    llvm::SmallVector<llvm::StringRef> macro_parameters;
    bool after_line_export = false;

    /// Open brackets, each the bracket depth it opened at.
    llvm::SmallVector<std::uint32_t> brackets;
    std::uint32_t depth = 0;
    bool names_template = false;
    bool after_template = false;
    clang::tok::TokenKind previous = clang::tok::unknown;

    struct AttributeList {
        /// The bracket depth of the names.
        std::uint32_t depth;
        /// Whether the list opened yet.
        bool entered = false;
    };
    std::optional<AttributeList> attribute;

    /// The token of the `#define` name being lexed, and what its
    /// replacement list holds so far.
    std::optional<std::size_t> definition;
    bool definition_empty = true;
    bool definition_control = false;

    std::size_t next_span = 0;
    std::vector<SemanticToken> tokens;
};

}  // namespace

auto semantic_token_stream(llvm::StringRef content,
                           const clang::LangOptions& lang_opts,
                           const TokenFacts& facts,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken> {
    return TokenStream(content, lang_opts, facts, options).build();
}

}  // namespace clice::feature
