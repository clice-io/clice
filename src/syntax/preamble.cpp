module clice;

import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.preamble;

namespace clice {

std::uint32_t preamble_bound(llvm::StringRef content, std::uint32_t limit) {
    Lexer lexer(content, dialect_of());
    auto spelled = [&](const Token& token, llvm::StringRef word) {
        return token.is_identifier() && token.text(content) == word;
    };
    // A global module fragment belongs to the preamble up to the module
    // declaration, declarations written in it too: clang accepts the module
    // declaration only first in the body or after a fragment it parsed
    // itself, which the PCH does not carry. A fragment cut at `limit` before
    // a declaration of its own leaves that declaration to the body, so the
    // file parses without a PCH; one reaching no module declaration ends at
    // its directives, as any preamble does.
    std::uint32_t directives = 0;
    std::uint32_t before_limit = 0;
    bool fragment = false;
    bool code = false;
    bool code_past_limit = false;
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        // The eod ending a line lexed as a directive.
        if(!token.is_at_start_of_line) {
            continue;
        }
        bool past_limit = token.range.begin >= limit;
        if(past_limit && !fragment) {
            break;
        }
        bool line_code = false;
        std::uint32_t end = 0;
        if(token.kind == clang::tok::hash) {
            lexer.advance_until(clang::tok::eod);
            end = lexer.last().range.end;
        } else if(spelled(token, "module") && lexer.next().kind == clang::tok::semi) {
            fragment = true;
            end = lexer.advance().range.end;
        } else if(!fragment) {
            break;
        } else if(spelled(token, "export") ? spelled(lexer.next(), "module")
                                           : spelled(token, "module")) {
            return code_past_limit ? 0 : before_limit;
        } else {
            line_code = true;
            end = token.range.end;
            while(!lexer.next().is_eof() && !lexer.next().is_at_start_of_line) {
                if(auto next = lexer.advance(); !next.is_eod()) {
                    end = next.range.end;
                }
            }
        }
        if(past_limit) {
            code_past_limit |= line_code;
            continue;
        }
        code |= line_code;
        before_limit = end;
        if(!code) {
            directives = end;
        }
    }
    return directives;
}

std::uint32_t finished_preamble_bound(llvm::StringRef content, std::uint32_t bound) {
    // The full (NUL-terminated) content is lexed; `bound` applies as a
    // logical end — tokens starting at or past it belong to the body.
    Lexer lexer(content, dialect_of());

    // Tokens of the current logical line; eod tokens only act as line ends.
    llvm::SmallVector<Token, 8> line;

    // A header-name directive (#include, #embed, ...) is complete once it
    // has a terminated filename argument (or a macro identifier standing in
    // for one).
    auto line_complete = [&] {
        if(!line.front().is_directive_hash() || line.size() < 2 || !line[1].is_identifier() ||
           !takes_header_name(line[1].text(content))) {
            return true;
        }
        if(line.size() < 3) {
            return false;
        }
        // A filename token's spelling may begin with line splices; skip
        // them before inspecting the delimiter.
        auto argument = line[2].text(content).ltrim("\\\r\n \t");
        if(argument.starts_with("\"")) {
            return argument.size() >= 2 && argument.ends_with("\"");
        }
        if(argument.starts_with("<")) {
            return argument.ends_with(">");
        }
        return true;
    };

    std::uint32_t complete = 0;
    while(true) {
        auto token = lexer.advance();
        bool at_end = token.is_eof() || token.range.begin >= bound;
        if((at_end || token.is_at_start_of_line) && !line.empty()) {
            if(!line_complete()) {
                return complete;
            }
            complete = line.back().range.end;
            line.clear();
        }
        if(at_end) {
            break;
        }
        if(!token.is_eod()) {
            line.push_back(token);
        }
    }

    return bound;
}

bool preamble_uses_macros(llvm::StringRef content, std::uint32_t bound) {
    Lexer lexer(content, dialect_of());
    while(true) {
        auto token = lexer.advance();
        if(token.is_eof() || token.range.begin >= bound) {
            return false;
        }
        if(!token.is_directive_hash()) {
            continue;
        }
        auto name = lexer.advance();
        if(name.is_identifier() && ranges::contains(std::array{"define",
                                                               "undef",
                                                               "if",
                                                               "ifdef",
                                                               "ifndef",
                                                               "elif",
                                                               "elifdef",
                                                               "elifndef"},
                                                    name.text(content))) {
            return true;
        }
    }
}

bool has_pragma_once(llvm::StringRef content, std::uint32_t bound) {
    // FIXME: lexical: a `#pragma once` in a branch the compile skips counts,
    // and `_Pragma("once")` does not.
    Lexer lexer(content, dialect_of());
    for(auto token = lexer.advance(); !token.is_eof() && token.range.begin < bound;
        token = lexer.advance()) {
        if(!token.is_directive_hash()) {
            continue;
        }
        auto keyword = lexer.advance();
        if(keyword.is_eod()) {
            continue;
        }
        auto argument = lexer.advance();
        if(directive_kind(keyword.text(content), argument.is_eod() ? "" : argument.text(content)) ==
           LexicalInfo::Directive::Kind::PragmaOnce) {
            return true;
        }
    }
    return false;
}

}  // namespace clice
