module clice;

import :syntax.lexer;
import :syntax.lexical_scan;
import :syntax.preamble;

namespace clice {

namespace {

/// preamble_bound, the pragma stacks aside.
std::uint32_t directive_bound(llvm::StringRef content, std::uint32_t limit) {
    Lexer lexer(content, dialect_of());
    auto spelled = [&](const Token& token, llvm::StringRef word) {
        return token.is_identifier() && token.text(content) == word;
    };
    // A global module fragment belongs to the preamble up to the module
    // declaration, declarations written in it too: clang accepts the module
    // declaration only as the first declaration it parses itself, those of
    // the PCH not counting. A fragment cut at `limit` before a declaration
    // of its own, or before an include, leaves declarations to the body, so
    // the file parses without a PCH; one reaching no module declaration ends
    // at its directives, as any preamble does.
    std::uint32_t directives = 0;
    std::uint32_t before_limit = 0;
    bool fragment = false;
    bool code = false;
    bool declares_past_limit = false;
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(!token.is_at_start_of_line) {
            continue;
        }
        bool past_limit = token.range.begin >= limit;
        if(past_limit && !fragment) {
            break;
        }
        bool line_code = false;
        bool includes = false;
        std::uint32_t end = 0;
        if(token.kind == clang::tok::hash) {
            auto name = lexer.next();
            includes = spelled(name, "include") || spelled(name, "include_next") ||
                       spelled(name, "import");
            /// For preprocessor directive, consume the whole directive: the
            /// bound is the end of the token before the eod.
            lexer.advance_until(clang::tok::eod);
            end = lexer.last().range.end;
        } else if(spelled(token, "module") && lexer.next().kind == clang::tok::semi) {
            fragment = true;
            end = lexer.advance().range.end;
        } else if(!fragment) {
            break;
        } else if(spelled(token, "export") ? spelled(lexer.next(), "module")
                                           : spelled(token, "module")) {
            return declares_past_limit ? 0 : before_limit;
        } else {
            line_code = true;
            end = token.range.end;
        }
        // The rest of a line that is no directive is code, after `module;`
        // too.
        while(token.kind != clang::tok::hash && !lexer.next().is_eof() &&
              !lexer.next().is_at_start_of_line) {
            if(auto next = lexer.advance(); !next.is_eod()) {
                line_code = true;
                end = next.range.end;
            }
        }
        if(past_limit) {
            declares_past_limit |= line_code || includes;
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

/// The stack a `#pragma` directive's tokens push to or pop from, among
/// those a PCH does not keep, and whether it pushes.
std::optional<std::pair<std::string, bool>> pragma_stack(llvm::ArrayRef<Token> directive,
                                                         llvm::StringRef content) {
    auto word = [&](std::size_t i) {
        return i < directive.size() ? directive[i].text(content) : llvm::StringRef();
    };
    auto push_or_pop = [&](std::string stack, llvm::StringRef action, llvm::StringRef push) {
        return std::optional(std::pair{std::move(stack), action == push});
    };
    if(word(0) != "pragma") {
        return std::nullopt;
    }
    auto name = word(1);
    if(name == "push_macro" || name == "pop_macro") {
        return push_or_pop("macro " + word(3).str(), name, "push_macro");
    }
    if(name == "pack" && (word(3) == "push" || word(3) == "pop")) {
        return push_or_pop("pack", word(3), "push");
    }
    if(name == "GCC" && word(2) == "visibility" && (word(3) == "push" || word(3) == "pop")) {
        return push_or_pop("visibility", word(3), "push");
    }
    if(name == "clang" && word(2) == "assume_nonnull" && (word(3) == "begin" || word(3) == "end")) {
        return push_or_pop("assume_nonnull", word(3), "begin");
    }
    if(name == "clang" && word(2) == "attribute") {
        // `push`, or `<namespace>.push`, whose pop names the namespace too.
        if(word(3) == "push" || word(3) == "pop") {
            return push_or_pop("attribute", word(3), "push");
        }
        if(word(4) == "." && (word(5) == "push" || word(5) == "pop")) {
            return push_or_pop("attribute " + word(3).str(), word(5), "push");
        }
    }
    return std::nullopt;
}

/// Where the earliest pragma push before `bound` starts that no pop
/// before `bound` matches.
std::optional<std::uint32_t> open_push(llvm::StringRef content, std::uint32_t bound) {
    struct Push {
        std::uint32_t offset;
        std::string stack;
    };

    llvm::SmallVector<Push> open;
    llvm::SmallVector<Token, 8> directive;
    Lexer lexer(content, dialect_of());
    for(auto token = lexer.advance(); !token.is_eof() && token.range.begin < bound;
        token = lexer.advance()) {
        if(!token.is_directive_hash()) {
            continue;
        }
        directive.clear();
        while(!lexer.next().is_eof() && !lexer.next().is_eod()) {
            directive.push_back(lexer.advance());
        }
        auto stack = pragma_stack(directive, content);
        if(!stack) {
            continue;
        }
        if(stack->second) {
            open.push_back({token.range.begin, std::move(stack->first)});
            continue;
        }
        auto pushed = views::reverse(open);
        if(auto last = ranges::find(pushed, stack->first, &Push::stack); last != pushed.end()) {
            open.erase(std::next(last).base());
        }
    }
    if(open.empty()) {
        return std::nullopt;
    }
    return open.front().offset;
}

}  // namespace

std::uint32_t preamble_bound(llvm::StringRef content, std::uint32_t limit) {
    // clang keeps none of these stacks in a PCH: a push the preamble ends
    // with is lost to the body, whose pop then finds nothing (the min/max
    // macros Windows code pushes around its includes). The preamble ends
    // before it.
    // LIMITATION: a push in a header the preamble includes, popped in the
    // file (pshpack1.h included directly), is not seen: the pop warns.
    auto bound = directive_bound(content, limit);
    while(auto push = open_push(content, bound)) {
        bound = directive_bound(content, *push);
    }
    return bound;
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

bool preamble_declares(llvm::StringRef content, std::uint32_t bound) {
    Lexer lexer(content, dialect_of());
    for(auto token = lexer.advance(); !token.is_eof() && token.range.begin < bound;
        token = lexer.advance()) {
        if(token.is_directive_hash()) {
            lexer.advance_until(clang::tok::eod);
            continue;
        }
        // The lexer ends `module;` as it does a directive.
        if(token.is_eod()) {
            continue;
        }
        bool opens_fragment = token.is_at_start_of_line && token.is_identifier() &&
                              token.text(content) == "module" &&
                              lexer.next().kind == clang::tok::semi;
        if(!opens_fragment) {
            return true;
        }
        lexer.advance();
    }
    return false;
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
