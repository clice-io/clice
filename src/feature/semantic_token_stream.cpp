module;

#include "support/prelude.h"

module clice;

import :feature.semantic_token_stream;
import :feature.semantic_tokens;
import :semantic.token_class;
import :syntax.doc_comment;
import :syntax.documentation;
import :syntax.source_text;
import :syntax.token;

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

bool is_char_literal(clang::tok::TokenKind kind) {
    return kind == clang::tok::char_constant || kind == clang::tok::wide_char_constant ||
           kind == clang::tok::utf8_char_constant || kind == clang::tok::utf16_char_constant ||
           kind == clang::tok::utf32_char_constant;
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
            return run(2, text.size(), llvm::isHexDigit);
        }
        case 'u':
        case 'U': {
            if(text.size() > 2 && text[2] == '{') {
                return braced(3);
            }
            return run(2, text[1] == 'u' ? 4 : 8, llvm::isHexDigit);
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
/// allowed. Zero for a field left open, which outside a raw string an
/// escape or the closing quote also ends.
std::size_t replacement_field_length(llvm::StringRef text, bool raw) {
    std::uint32_t depth = 0;
    for(std::size_t i = 0; i < text.size(); i += 1) {
        if(text[i] == '{') {
            depth += 1;
        } else if(text[i] == '}') {
            depth -= 1;
            if(depth == 0) {
                return i + 1;
            }
        } else if(!raw && (text[i] == '\\' || text[i] == '"')) {
            return 0;
        }
    }
    return 0;
}

/// A conversion specification of a format string being read, from its `%`.
struct ConversionCursor {
    llvm::StringRef text;
    std::size_t i = 1;

    bool take(char c) {
        if(i < text.size() && text[i] == c) {
            i += 1;
            return true;
        }
        return false;
    }

    void skip(llvm::StringRef set) {
        while(i < text.size() && set.contains(text[i])) {
            i += 1;
        }
    }

    /// A POSIX argument selector, the `2$` of `%2$d`.
    void select() {
        auto digits = i;
        while(digits < text.size() && llvm::isDigit(text[digits])) {
            digits += 1;
        }
        if(digits > i && digits < text.size() && text[digits] == '$') {
            i = digits + 1;
        }
    }

    /// C23's bit-width length modifier, the `w32` of `%w32d` or the `wf8`
    /// of `%wf8u`.
    void width_length() {
        if(take('w')) {
            take('f');
            skip("0123456789");
        }
    }

    /// The specification's length if one of `conversions` ends it, else zero.
    std::size_t end(llvm::StringRef conversions) const {
        return i < text.size() && conversions.contains(text[i]) ? i + 1 : 0;
    }
};

/// The length of the printf conversion specification at `text[0]`, a `%`:
/// argument, flags, width, precision, length and conversion. Zero for none.
std::size_t printf_conversion_length(llvm::StringRef text) {
    ConversionCursor cursor{.text = text};
    cursor.select();
    cursor.skip("-+ #0'");
    if(cursor.take('*')) {
        cursor.select();
    } else {
        cursor.skip("0123456789");
    }
    if(cursor.take('.')) {
        if(cursor.take('*')) {
            cursor.select();
        } else {
            cursor.skip("0123456789");
        }
    }
    cursor.width_length();
    cursor.skip("hljztLqI0123456789");
    return cursor.end("diouxXbBeEfFgGaAcspnCSm");
}

/// The length of the scanf conversion specification at `text[0]`, a `%`:
/// argument, assignment suppression, width, allocation, length and
/// conversion, a scanset `[...]` included. Zero for none.
std::size_t scanf_conversion_length(llvm::StringRef text) {
    ConversionCursor cursor{.text = text};
    cursor.select();
    cursor.take('*');
    cursor.skip("0123456789");
    cursor.take('m');
    cursor.width_length();
    cursor.skip("hljztLq");
    if(cursor.take('[')) {
        // A `]` first in the set, after the `^` too, is one of its characters.
        cursor.take('^');
        cursor.take(']');
        auto close = text.find(']', cursor.i);
        return close == llvm::StringRef::npos ? 0 : close + 1;
    }
    return cursor.end("diouxXbeEfFgGaAcspnCS");
}

/// Whether `name` is a check clang evaluates in a condition, whose operand
/// is no macro: `__has_cpp_attribute(nodiscard)`, `__is_target_os(darwin)`,
/// `__building_module(Core)`.
bool is_check(llvm::StringRef name) {
    return name.starts_with("__has_") || name.starts_with("__is_") || name == "__building_module";
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
        return hex ? llvm::isHexDigit(c) : binary ? (c == '0' || c == '1') : llvm::isDigit(c);
    };
    while(i < text.size()) {
        char c = text[i];
        if(digit(c) || c == '.' || (c == '\'' && i + 1 < text.size() && digit(text[i + 1]))) {
            i += 1;
        } else if((hex ? (c == 'p' || c == 'P') : (!binary && (c == 'e' || c == 'E'))) &&
                  i + 1 < text.size() &&
                  (llvm::isDigit(text[i + 1]) || text[i + 1] == '+' || text[i + 1] == '-')) {
            // The exponent ends the digits: a hexadecimal one's `f` is a
            // suffix.
            i += 2;
            while(i < text.size() &&
                  (llvm::isDigit(text[i]) ||
                   (text[i] == '\'' && i + 1 < text.size() && llvm::isDigit(text[i + 1])))) {
                i += 1;
            }
            return i;
        } else {
            break;
        }
    }
    return i;
}

class TokenStream {
public:
    TokenStream(SourceText& source, const TokenFacts& facts, const SemanticTokensOptions& options) :
        source(source), stream(source.lexical().tokens), content(source.text()),
        lang_opts(source.dialect()), facts(facts), options(options) {}

    auto build() -> std::vector<SemanticToken> {
        for(at = 0; at < stream.size(); at += 1) {
            const auto& token = stream[at];
            if(token.is_eod()) {
                end_directive();
                continue;
            }
            if(token.kind == clang::tok::comment) {
                comment(token.range);
                continue;
            }
            doc_commands.end_run();
            classify(token);
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
        /// In the parentheses of a check such as `__has_include`: what it
        /// tests is no macro.
        Check,
        /// Anywhere else in a directive.
        Other,
    };

    struct Piece {
        TokenClass classified;
        /// Sent with lexical tokens off too: what the editor's syntax
        /// highlighting cannot tell.
        bool semantic;
    };

    /// The token after the one being classified.
    /// FIXME: a kept comment counts as that token: `default /* c */:` gets no
    /// controlFlow, and `module /* c */ m;` / `import /* c */ m;` no keyword.
    Token next() const {
        return at + 1 < stream.size() ? stream[at + 1] : Token{.kind = clang::tok::eof};
    }

    void classify(const Token& token) {
        if(directive == Directive::Body) {
            definition_empty = false;
        }
        auto spelling = token.text(content);
        auto kind = source.kind(token);
        bool word = clang::tok::isAnyIdentifier(kind) || clang::tok::getKeywordSpelling(kind);

        if(clang::tok::isStringLiteral(kind) || is_char_literal(kind)) {
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
            emit({middle, token.range.end},
                 {{.kind = closes ? Kind::Bracket : Kind::Operator}, closes});
            after(kind, false);
            return;
        }

        auto piece = lexical(token, kind, spelling, word);
        if(auto semantic = semantic_of(token, spelling, word)) {
            piece = {*semantic, true};
        } else if(word && attribute && depth == attribute->depth && kind != clang::tok::kw_using) {
            piece = {{.kind = Kind::Attribute}, true};
        }
        piece = bracket(kind, piece);
        follow_attribute(kind);
        emit(token.range, piece);
        if(piece.classified.kind == Kind::Macro && piece.classified.has(Modifier::Definition) &&
           !tokens.empty() && tokens.back().range.begin == token.range.begin) {
            definition = tokens.size() - 1;
        }
        after(kind,
              piece.classified.has(Modifier::Template) || piece.classified.kind == Kind::Concept);
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
        attribute.reset();
    }

    /// Where attribute names are written: in `[[...]]`, in the double
    /// parentheses of `__attribute__`, in those of `__declspec`; their
    /// arguments sit deeper.
    void follow_attribute(clang::tok::TokenKind kind) {
        if(attribute) {
            if(depth >= attribute->depth) {
                attribute->entered = true;
            } else if(attribute->entered || kind != clang::tok::l_paren) {
                // Out of the list, or no list after all: a macro named
                // like the keyword (`#define __attribute__(x)`).
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
    Piece lexical(const Token& token,
                  clang::tok::TokenKind kind,
                  llvm::StringRef spelling,
                  bool word) {
        Piece piece{.semantic = false};
        if(clang::tok::getKeywordSpelling(kind)) {
            piece.classified = keyword_class(kind);
            if(kind == clang::tok::kw_default && next().kind == clang::tok::colon) {
                piece.classified.modifiers |= mask(Modifier::ControlFlow);
            }
        } else if(clang::tok::getPunctuatorSpelling(kind)) {
            // Alternative spellings (`and`, `not`) lex as their punctuators.
            if(!spelling.empty() && llvm::isAlpha(spelling.front())) {
                piece.classified = {.kind = Kind::Keyword, .modifiers = mask(Modifier::Operator)};
            } else if(is_operator(kind)) {
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
            if(lang_opts.CPlusPlusModules && line_head &&
               (spelling == "module" || spelling == "import")) {
                auto following = next();
                bool opens = spelling == "module"
                                 ? ranges::contains(std::array{clang::tok::raw_identifier,
                                                               clang::tok::colon,
                                                               clang::tok::semi},
                                                    following.kind)
                                 : ranges::contains(std::array{clang::tok::raw_identifier,
                                                               clang::tok::colon,
                                                               clang::tok::less,
                                                               clang::tok::header_name,
                                                               clang::tok::string_literal},
                                                    following.kind);
                if(opens) {
                    return {{.kind = Kind::Keyword}, true};
                }
            }
            if(lang_opts.CPlusPlus && is_override_keyword(spelling)) {
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
                } else if(ranges::contains(
                              std::array{"ifdef", "ifndef", "elifdef", "elifndef", "undef"},
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
                bool function_like =
                    next().kind == clang::tok::l_paren && next().range.begin == token.range.end;
                piece = {
                    {.kind = Kind::Macro,
                     .modifiers = mask(Modifier::Definition) |
                                  (function_like ? mask(Modifier::Callable) : 0)},
                    true
                };
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
                definition_control |= leaves_enclosing(kind);
                if(word && ranges::contains(macro_parameters, spelling)) {
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
                    piece.classified = {.kind = Kind::Keyword,
                                        .modifiers = mask(Modifier::Operator)};
                    directive = Directive::Defined;
                } else if(is_check(spelling)) {
                    piece.classified = {.kind = Kind::Keyword,
                                        .modifiers = mask(Modifier::Operator)};
                    directive = Directive::Check;
                    check_depth = depth;
                } else if(kind == clang::tok::identifier) {
                    piece = {{.kind = Kind::Macro}, true};
                }
                break;
            }
            case Directive::Check: {
                // `bracket` has not left the parenthesis yet.
                if(kind == clang::tok::r_paren && depth == check_depth + 1) {
                    directive = Directive::Condition;
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
                if(names_template || piece.classified.kind == Kind::Bracket) {
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

        std::optional<FormatStyle> format;
        if(auto it = facts.formats.find(range.begin); it != facts.formats.end()) {
            format = it->second;
        }

        auto at = [&](std::size_t offset) {
            return range.begin + static_cast<std::uint32_t>(offset);
        };
        if(open > 0) {
            emit(
                {
                    range.begin,
                    at(open)
            },
                {{.kind = base, .modifiers = mask(Modifier::Affix)}, false});
        }
        std::size_t segment = open;
        auto flush = [&](std::size_t until) {
            if(until > segment) {
                emit({at(segment), at(until)}, {{.kind = base}, false});
            }
        };
        // A raw string's delimiter is no part of its text.
        auto begin = raw ? text.find('(', open) + 1 : open + 1;
        auto end = raw ? text.rfind(')', close) : close;
        for(std::size_t i = begin; i < end;) {
            std::size_t length = 0;
            Piece piece;
            if(!raw && text[i] == '\\') {
                length = std::min(escape_length(text.substr(i)), end - i);
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Std && (text[i] == '{' || text[i] == '}') &&
                      i + 1 < end && text[i + 1] == text[i]) {
                length = 2;
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Std && text[i] == '{') {
                length = replacement_field_length(text.slice(i, end), raw);
                piece = {{.kind = Kind::FormatSpecifier}, true};
            } else if((format == FormatStyle::Printf || format == FormatStyle::Scanf) &&
                      text[i] == '%' && i + 1 < end && text[i + 1] == '%') {
                length = 2;
                piece = {{.kind = Kind::Escape}, false};
            } else if(format == FormatStyle::Printf && text[i] == '%') {
                length = printf_conversion_length(text.slice(i, end));
                piece = {{.kind = Kind::FormatSpecifier}, true};
            } else if(format == FormatStyle::Scanf && text[i] == '%') {
                length = scanf_conversion_length(text.slice(i, end));
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
            suffix({at(close + 1), range.end}, base, user_defined(range.begin));
        }
    }

    void number(LocalSourceRange range) {
        auto text = content.slice(range.begin, range.end);
        auto split = number_suffix(text);
        auto at = range.begin + static_cast<std::uint32_t>(split);
        emit({range.begin, at}, {{.kind = Kind::Number}, false});
        if(at < range.end) {
            suffix({at, range.end}, Kind::Number, user_defined(range.begin));
        }
    }

    /// Whether the facts take the literal at `begin` for a user-defined one.
    bool user_defined(std::uint32_t begin) const {
        auto it = facts.semantic.find(begin);
        return it != facts.semantic.end() && it->second.has(Modifier::UserDefined);
    }

    void suffix(LocalSourceRange range, Kind base, bool user_literal) {
        TokenClass classified{.kind = base, .modifiers = mask(Modifier::Affix)};
        if(user_literal) {
            classified.modifiers |= mask(Modifier::UserDefined);
        }
        emit(range, {classified, user_literal});
    }

    /// A comment; a documentation comment's commands as keywords and the
    /// names `@param` and `@tparam` document as what they name.
    void comment(LocalSourceRange range) {
        auto text = content.slice(range.begin, range.end);
        // A blank line ends a documentation comment, a raw block left open
        // in it too.
        if(content.slice(comment_end, range.begin).count('\n') > 1) {
            doc_commands.end_run();
        }
        comment_end = range.end;
        if(!is_doc_comment(text)) {
            doc_commands.end_run();
            emit(range, {{.kind = Kind::Comment}, false});
            return;
        }
        auto mark = [&](std::size_t begin, std::size_t end, Kind kind, bool semantic) {
            auto at = [&](std::size_t offset) {
                return range.begin + static_cast<std::uint32_t>(offset);
            };
            TokenClass classified{.kind = kind, .modifiers = mask(Modifier::Documentation)};
            emit({at(begin), at(end)}, {classified, semantic});
        };
        std::size_t segment = 0;
        for(auto& part: doc_commands.scan(text)) {
            using PartKind = DocCommandScanner::Part::Kind;
            mark(segment, part.begin, Kind::Comment, false);
            auto kind = part.kind == PartKind::Command     ? Kind::Keyword
                        : part.kind == PartKind::Parameter ? Kind::Parameter
                                                           : Kind::TypeParameter;
            mark(part.begin, part.end, kind, part.kind != PartKind::Command);
            segment = part.end;
        }
        mark(segment, text.size(), Kind::Comment, false);
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
        tokens.push_back(
            {.range = range, .kind = classified.kind, .modifiers = classified.modifiers});
    }

    SourceText& source;
    llvm::ArrayRef<Token> stream;
    /// The index in `stream` of the token being classified.
    std::size_t at = 0;
    llvm::StringRef content;
    const clang::LangOptions& lang_opts;
    const TokenFacts& facts;
    const SemanticTokensOptions& options;

    Directive directive = Directive::None;
    llvm::SmallVector<llvm::StringRef> macro_parameters;
    /// The bracket depth of a condition's check.
    std::uint32_t check_depth = 0;
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

    /// The run of documentation comments being read, whose raw text — code,
    /// a formula, hidden text — may end on a later line of a `///` block
    /// (`endcode`).
    DocCommandScanner doc_commands;
    /// Where the last comment ended.
    std::uint32_t comment_end = 0;

    std::size_t next_span = 0;
    std::vector<SemanticToken> tokens;
};

}  // namespace

bool names_token(clang::tok::TokenKind kind, const TokenClass& token) {
    return kind == clang::tok::identifier || kind == clang::tok::tilde ||
           (token.kind == SemanticTokenKind::Operator &&
            (clang::tok::getPunctuatorSpelling(kind) || clang::tok::isLiteral(kind)));
}

bool is_override_keyword(llvm::StringRef spelling) {
    return spelling == "override" || spelling == "final" || spelling == "sealed";
}

auto semantic_token_stream(SourceText& source,
                           const TokenFacts& facts,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken> {
    return TokenStream(source, facts, options).build();
}

}  // namespace clice::feature
