module;

#include "modules/prelude.h"

module clice:analysis.text;

import :support.text;
import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.token;

namespace clice::analysis {

/// A file's lines and what its lexical scan finds on each, for the analyses
/// that read and rewrite sources a line at a time.
struct Text {
    enum class Line : std::uint8_t {
        /// Whitespace, comments, and an include guard's directives.
        Blank,
        Code,
        /// The first line of a preprocessor directive.
        Directive,
        /// A directive's continuation line.
        Continued,
    };

    llvm::SmallVector<llvm::StringRef, 0> lines;
    std::vector<std::uint32_t> starts;
    std::vector<Line> kinds;

    /// Per line: the keyword of the directive it starts, its tokens after
    /// the keyword, and an include's operand as written, `<...>` or `"..."`.
    std::vector<llvm::StringRef> keywords;
    std::vector<llvm::SmallVector<llvm::StringRef, 2>> arguments;
    std::vector<llvm::StringRef> operands;

    /// Per directive: its last line.
    std::vector<std::uint32_t> ends;

    /// Per line: the preprocessor conditions open around it, an include
    /// guard's apart.
    std::vector<std::uint32_t> depths;

    /// The tokens outside directives and the line of each.
    std::vector<Token> tokens;
    std::vector<std::uint32_t> token_lines;

    /// The `#ifndef`, `#define` and `#endif` of an include guard, which a
    /// module unit does without.
    llvm::SmallVector<std::uint32_t, 3> guard;

    llvm::StringRef content;

    /// `content` ends at a NUL terminator, as the lexer requires.
    explicit Text(llvm::StringRef content) : content(content) {
        content.split(lines, '\n');
        // The newline ending the last line starts none.
        if(content.ends_with("\n")) {
            lines.pop_back();
        }
        std::uint32_t offset = 0;
        // A byte order mark the lexer skips, and generated lines would leave
        // in the middle of the file.
        if(!lines.empty() && lines.front().starts_with("\xEF\xBB\xBF")) {
            lines.front() = lines.front().drop_front(3);
            offset = 3;
        }
        for(auto line: lines) {
            starts.push_back(offset);
            offset += static_cast<std::uint32_t>(line.size()) + 1;
        }
        auto count = lines.size();
        kinds.assign(count, Line::Blank);
        keywords.resize(count);
        arguments.resize(count);
        operands.resize(count);
        ends.resize(count);

        const auto& dialect = raw_dialect(clang::Language::CXX, clang::LangStandard::lang_cxx23);
        auto lexical = lexical_scan(content, dialect);
        auto directive = lexical.directives.begin();
        for(std::uint32_t index = 0; index < lexical.tokens.size(); index += 1) {
            if(directive != lexical.directives.end() && index == directive->first) {
                auto line = line_of(directive->range.begin);
                kinds[line] = Line::Directive;
                for(const auto& part: lexical.words(*directive)) {
                    if(part.is_pp_keyword) {
                        keywords[line] = part.text(content);
                    } else if(part.is_header_name() && operands[line].empty()) {
                        operands[line] = part.text(content);
                    } else {
                        arguments[line].push_back(part.text(content));
                    }
                }
                auto last = line_of(directive->range.end);
                ends[line] = last;
                for(auto continued = line + 1; continued <= last; continued += 1) {
                    kinds[continued] = Line::Continued;
                }
                index = directive->last;
                ++directive;
                continue;
            }
            const auto& token = lexical.tokens[index];
            if(token.kind == clang::tok::comment) {
                continue;
            }
            auto line = line_of(token.range.begin);
            if(kinds[line] == Line::Blank) {
                kinds[line] = Line::Code;
            }
            tokens.push_back(token);
            token_lines.push_back(line);
        }
        if(const auto& found = lexical.include_guard) {
            for(auto member: {found->opening, found->definition, found->closing}) {
                auto line = line_of(lexical.directives[member].range.begin);
                guard.push_back(line);
                for(auto part = line; part <= ends[line]; part += 1) {
                    kinds[part] = Line::Blank;
                }
            }
        }
        std::uint32_t depth = 0;
        for(std::uint32_t line = 0; line < count; line += 1) {
            auto kind = directive_kind(line);
            if(kind == LexicalInfo::Directive::Kind::EndIf && depth != 0) {
                depth -= 1;
            }
            depths.push_back(depth);
            if(kind == LexicalInfo::Directive::Kind::If) {
                depth += 1;
            }
        }
    }

    std::uint32_t line_of(std::uint32_t offset) const {
        return static_cast<std::uint32_t>(std::ranges::upper_bound(starts, offset) -
                                          starts.begin() - 1);
    }

    /// The kind of the directive `line` starts; Other off a directive.
    LexicalInfo::Directive::Kind directive_kind(std::uint32_t line) const {
        if(kinds[line] != Line::Directive) {
            return LexicalInfo::Directive::Kind::Other;
        }
        return clice::directive_kind(keywords[line],
                                     arguments[line].empty() ? "" : arguments[line].front());
    }

    /// The line starting the directive `line` is a line of.
    std::optional<std::uint32_t> directive_line(std::uint32_t line) const {
        while(kinds[line] == Line::Continued) {
            line -= 1;
        }
        return kinds[line] == Line::Directive ? std::optional(line) : std::nullopt;
    }

    bool pragma_once(std::uint32_t line) const {
        return directive_kind(line) == LexicalInfo::Directive::Kind::PragmaOnce;
    }

    /// Whether `line` writes the identifier `word`, outside its comments,
    /// literals and directives.
    bool names(std::uint32_t line, llvm::StringRef word) const {
        return std::ranges::any_of(tokens_on(line), [&](const Token& token) {
            return token.is_identifier() && token.text(content) == word;
        });
    }

    /// The directive `line` starts, its continuation lines and those a block
    /// comment it opens runs on to joined.
    std::string directive_text(std::uint32_t line) const {
        auto begin = starts[line];
        auto end = ends[line] + 1 < starts.size() ? starts[ends[line] + 1] : content.size();
        return unspliced(content.slice(begin, end).rtrim("\r\n"));
    }

    /// The first line of the body: past the leading directives, before a
    /// conditional block the body continues; the comments after the last of
    /// them document the body.
    std::uint32_t preamble_end() const {
        llvm::SmallVector<std::uint32_t> opened;
        std::uint32_t last = 0;
        for(std::uint32_t line = 0; line < lines.size(); line += 1) {
            if(kinds[line] == Line::Code) {
                break;
            }
            if(kinds[line] == Line::Directive) {
                auto keyword = keywords[line];
                if(keyword == "if" || keyword == "ifdef" || keyword == "ifndef") {
                    opened.push_back(line);
                } else if(keyword == "endif" && !opened.empty()) {
                    opened.pop_back();
                }
                last = line + 1;
            } else if(kinds[line] == Line::Continued) {
                last = line + 1;
            }
        }
        return opened.empty() ? last : opened.front();
    }

    /// The tokens on `line`.
    llvm::ArrayRef<Token> tokens_on(std::uint32_t line) const {
        auto begin = std::ranges::lower_bound(token_lines, line) - token_lines.begin();
        auto end = std::ranges::upper_bound(token_lines, line) - token_lines.begin();
        return llvm::ArrayRef(tokens).slice(begin, end - begin);
    }

    /// The tokens on lines `first` through `last`.
    llvm::ArrayRef<Token> tokens_on(std::uint32_t first, std::uint32_t last) const {
        auto begin = std::ranges::lower_bound(token_lines, first) - token_lines.begin();
        auto end = std::ranges::upper_bound(token_lines, last) - token_lines.begin();
        return llvm::ArrayRef(tokens).slice(begin, end - begin);
    }

    /// The tokens on lines `first` through `last`, spaced: the code without
    /// its comments.
    std::string code(std::uint32_t first, std::uint32_t last) const {
        std::string text;
        for(auto& token: tokens_on(first, last)) {
            if(!text.empty()) {
                text += ' ';
            }
            text += token.text(content);
        }
        return text;
    }

    /// The tokens past a leading template head `template <...>`, all of
    /// them when there is none; nothing when the head does not close.
    std::optional<llvm::ArrayRef<Token>> past_template_head(llvm::ArrayRef<Token> rest) const {
        if(rest.empty() || rest.front().text(content) != "template") {
            return rest;
        }
        int depth = 0;
        for(std::size_t i = 1; i < rest.size(); i += 1) {
            depth += rest[i].kind == clang::tok::less;
            depth -= rest[i].kind == clang::tok::greater;
            depth -= 2 * (rest[i].kind == clang::tok::greatergreater);
            if(depth <= 0) {
                return rest.drop_front(i + 1);
            }
        }
        return std::nullopt;
    }

    /// The lines of the forward declaration of a type on `line`, when they
    /// hold it and nothing else: `class C;`, `enum class E : int;`, and
    /// `template <typename T> struct S;` on one line or with the template
    /// head (and a requires-clause) on the lines before.
    std::optional<std::pair<std::uint32_t, std::uint32_t>>
        forward_declaration(std::uint32_t line) const {
        auto all = tokens_on(line);
        auto past = past_template_head(all);
        if(!past) {
            return std::nullopt;
        }
        auto rest = *past;
        auto text = [&](std::size_t i) {
            return i < rest.size() ? rest[i].text(content) : llvm::StringRef();
        };
        std::size_t i = 0;
        if(text(i) == "enum") {
            i += 1;
            if(text(i) == "class" || text(i) == "struct") {
                i += 1;
            }
        } else if(text(i) == "class" || text(i) == "struct" || text(i) == "union") {
            i += 1;
        } else {
            return std::nullopt;
        }
        if(i >= rest.size() || !rest[i].is_identifier()) {
            return std::nullopt;
        }
        i += 1;
        if(i < rest.size() && rest[i].kind == clang::tok::colon) {
            for(i += 1; i < rest.size() &&
                        (rest[i].is_identifier() || rest[i].kind == clang::tok::coloncolon);
                i += 1) {}
        }
        if(i + 1 != rest.size() || rest[i].kind != clang::tok::semi) {
            return std::nullopt;
        }
        auto first = line;
        // The lines above that end no statement or block lead up to it.
        for(auto previous = line; rest.size() == all.size() && previous != 0;) {
            previous -= 1;
            if(kinds[previous] == Line::Blank) {
                continue;
            }
            auto above = tokens_on(previous);
            if(kinds[previous] != Line::Code || above.back().kind == clang::tok::semi ||
               above.back().kind == clang::tok::l_brace ||
               above.back().kind == clang::tok::r_brace) {
                break;
            }
            if(above.front().text(content) == "template") {
                if(past_template_head(tokens_on(previous, line - 1))) {
                    first = previous;
                }
                break;
            }
        }
        return std::pair{first, line};
    }

    /// The token closing the brace `open` opens.
    std::optional<std::size_t> closing_brace(std::size_t open) const {
        int depth = 0;
        for(auto close = open; close < tokens.size(); close += 1) {
            depth += tokens[close].kind == clang::tok::l_brace;
            depth -= tokens[close].kind == clang::tok::r_brace;
            if(depth == 0) {
                return close;
            }
        }
        return std::nullopt;
    }

    /// The opening and closing lines of each anonymous namespace standing on
    /// lines of their own.
    llvm::DenseSet<std::uint32_t> anonymous_namespaces() const {
        llvm::DenseSet<std::uint32_t> found;
        for(std::size_t i = 0; i + 1 < tokens.size(); i += 1) {
            auto line = token_lines[i];
            if(tokens[i].text(content) != "namespace" ||
               tokens[i + 1].kind != clang::tok::l_brace || tokens_on(line).size() != 2) {
                continue;
            }
            if(auto close = closing_brace(i + 1);
               close && tokens_on(token_lines[*close]).size() == 1) {
                found.insert(line);
                found.insert(token_lines[*close]);
            }
        }
        return found;
    }

    /// The opening and closing lines of each named namespace left holding
    /// nothing once `dropped` goes, standing on lines of their own; one
    /// holding only such namespaces goes with them, one holding a directive
    /// stays.
    llvm::DenseSet<std::uint32_t>
        empty_namespaces(const llvm::DenseSet<std::uint32_t>& dropped) const {
        llvm::DenseSet<std::uint32_t> found;
        // From the last opening, so the namespaces inside one come first.
        for(auto i = tokens.size(); i > 0;) {
            i -= 1;
            auto open = token_lines[i];
            auto on_open = tokens_on(open);
            if(&on_open.front() != &tokens[i] || on_open.front().text(content) != "namespace" ||
               on_open.size() < 3 || on_open.back().kind != clang::tok::l_brace ||
               !std::ranges::all_of(on_open.drop_front().drop_back(), [](const auto& token) {
                   return token.is_identifier() || token.kind == clang::tok::coloncolon;
               })) {
                continue;
            }
            auto close = closing_brace(i + on_open.size() - 1);
            if(!close || tokens_on(token_lines[*close]).size() != 1) {
                continue;
            }
            auto empty = true;
            for(auto line = open + 1; empty && line < token_lines[*close]; line += 1) {
                empty =
                    kinds[line] == Line::Blank || dropped.contains(line) || found.contains(line);
            }
            if(empty) {
                found.insert(open);
                found.insert(token_lines[*close]);
            }
        }
        return found;
    }

    /// The offset of the return type of a definition of `main` at global
    /// scope, the last one: `int main(` in a string literal is no token.
    std::optional<std::uint32_t> main_definition() const {
        auto returned = [&](std::size_t i) {
            auto word = tokens[i].text(content);
            return word == "int" || word == "signed" || word == "auto";
        };
        std::optional<std::uint32_t> found;
        int depth = 0;
        for(std::size_t i = 0; i < tokens.size(); i += 1) {
            depth += tokens[i].kind == clang::tok::l_brace;
            depth -= tokens[i].kind == clang::tok::r_brace;
            if(depth == 0 && i != 0 && i + 1 < tokens.size() && tokens[i].text(content) == "main" &&
               tokens[i + 1].kind == clang::tok::l_paren && returned(i - 1)) {
                auto first = i - 1;
                while(first != 0 && returned(first - 1)) {
                    first -= 1;
                }
                found = tokens[first].range.begin;
            }
        }
        return found;
    }
};

}  // namespace clice::analysis
