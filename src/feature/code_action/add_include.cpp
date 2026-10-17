module;

#include "modules/prelude.h"

module clice;

import :compile.compilation;
import :feature.code_action.action;
import :syntax.lexer;
import :syntax.lexical_scan;

namespace clice::feature::action {

namespace {

/// The diagnostics whose subject is a name no visible declaration
/// provides.
bool unresolved_name(std::uint32_t id) {
    namespace diag = clang::diag;
    switch(id) {
        case diag::err_undeclared_var_use:
        case diag::err_undeclared_var_use_suggest:
        case diag::err_undeclared_use:
        case diag::err_undeclared_use_suggest:
        case diag::err_unknown_typename:
        case diag::err_unknown_typename_suggest:
        case diag::err_unknown_type_or_class_name_suggest:
        case diag::err_unknown_nested_typename_suggest:
        case diag::err_no_member:
        case diag::err_no_member_suggest:
        case diag::err_no_member_template:
        case diag::err_no_template:
        case diag::err_no_template_suggest:
        case diag::err_typename_nested_not_found:
        case diag::err_expected_class_or_namespace: return true;
        default: return false;
    }
}

/// The qualified name around the token at `offset`: the identifiers a
/// chain of `::` joins, the last one being the name looked up.
struct QualifiedName {
    std::string scope;
    std::string name;
    /// The chain's span in the focus file.
    LocalSourceRange range;
    /// The number of arguments of the call the name is the callee of.
    std::optional<std::size_t> arguments;
};

/// The number of arguments of the argument list `tokens` open; nullopt
/// when they open none, it is never closed, a bracket in it is closed by
/// another kind, or it holds a `<`: the text alone cannot tell a less-than
/// from a template argument list, whose commas separate no arguments.
std::optional<std::size_t> call_arguments(llvm::ArrayRef<clang::syntax::Token> tokens) {
    if(tokens.empty() || tokens.front().kind() != clang::tok::l_paren) {
        return std::nullopt;
    }
    std::size_t commas = 0;
    llvm::SmallVector<clang::tok::TokenKind, 4> closers;
    for(const auto& token: tokens) {
        switch(token.kind()) {
            case clang::tok::l_paren: closers.push_back(clang::tok::r_paren); break;
            case clang::tok::l_square: closers.push_back(clang::tok::r_square); break;
            case clang::tok::l_brace: closers.push_back(clang::tok::r_brace); break;
            case clang::tok::r_paren:
            case clang::tok::r_square:
            case clang::tok::r_brace:
                if(closers.pop_back_val() != token.kind()) {
                    return std::nullopt;
                }
                if(closers.empty()) {
                    return &token == &tokens[1] ? 0 : commas + 1;
                }
                break;
            case clang::tok::comma:
                if(closers.size() == 1) {
                    commas += 1;
                }
                break;
            case clang::tok::less: return std::nullopt;
            default: break;
        }
    }
    return std::nullopt;
}

/// None for a member named through `.` or `->`: the object's class
/// declares it, not a header.
std::optional<QualifiedName> qualified_name_at(CompilationUnit& unit, std::uint32_t offset) {
    auto& token_map = unit.focus_file().tokens();
    auto tokens = token_map.spelled();
    auto& SM = unit.context().getSourceManager();
    auto offset_of = [&](const clang::syntax::Token& token) {
        return token_map.offset(token);
    };
    // The token under the cursor, else the one the cursor sits right
    // after: editors ask at either end of a name.
    auto it = std::ranges::partition_point(tokens, [&](const clang::syntax::Token& token) {
        return offset_of(token) + token.length() <= offset;
    });
    if((it == tokens.end() || offset_of(*it) > offset) && it != tokens.begin() &&
       offset_of(*std::prev(it)) + std::prev(it)->length() == offset) {
        --it;
    }
    if(it == tokens.end() || it->kind() != clang::tok::identifier || offset_of(*it) > offset) {
        return std::nullopt;
    }
    auto index = static_cast<std::size_t>(it - tokens.begin());
    auto is_identifier = [&](std::size_t i) {
        return i < tokens.size() && tokens[i].kind() == clang::tok::identifier;
    };
    auto is_scope = [&](std::size_t i) {
        return i < tokens.size() && tokens[i].kind() == clang::tok::coloncolon;
    };
    auto first = index;
    while(first >= 2 && is_scope(first - 1) && is_identifier(first - 2)) {
        first -= 2;
    }
    auto access = first;
    if(access > 0 && tokens[access - 1].kind() == clang::tok::kw_template) {
        access -= 1;
    }
    if(access > 0 &&
       llvm::is_contained({clang::tok::period, clang::tok::arrow}, tokens[access - 1].kind())) {
        return std::nullopt;
    }
    auto last = index;
    while(is_scope(last + 1) && is_identifier(last + 2)) {
        last += 2;
    }
    QualifiedName result;
    for(auto i = first; i < last; i += 2) {
        result.scope += tokens[i].text(SM);
        result.scope += "::";
    }
    result.name = tokens[last].text(SM).str();
    result.range = {offset_of(tokens[first]), offset_of(tokens[last]) + tokens[last].length()};
    result.arguments = call_arguments(tokens.drop_front(last + 1));
    return result;
}

/// The header of a standard function the mapping leaves out because its
/// overloads live in different headers, told apart as include-cleaner does,
/// by the number of arguments of the call.
std::optional<clang::tooling::stdlib::Header>
    overloaded_standard_header(llvm::StringRef scope, const QualifiedName& name) {
    struct Overload {
        llvm::StringRef name;
        std::size_t arguments;
        llvm::StringRef header;
    };

    constexpr std::array overloads = {
        Overload{"move",   1, "<utility>"  },
        Overload{"move",   3, "<algorithm>"},
        Overload{"move",   4, "<algorithm>"},
        Overload{"remove", 1, "<cstdio>"   },
        Overload{"remove", 3, "<algorithm>"},
    };
    if(scope != "std::") {
        return std::nullopt;
    }
    for(const auto& overload: overloads) {
        if(overload.name == name.name && overload.arguments == name.arguments) {
            return clang::tooling::stdlib::Header::named(overload.header);
        }
    }
    return std::nullopt;
}

/// The start of the line after the comments opening the file, up to the
/// last one a blank line sets apart from what follows: a license header,
/// not the comment of the first declaration. 0 when there is none.
std::uint32_t after_leading_comments(llvm::StringRef content, const LexicalInfo& lexical) {
    std::uint32_t offset = 0;
    auto& tokens = lexical.tokens;
    for(std::size_t index = 0; index < tokens.size() && tokens[index].kind == clang::tok::comment;
        index += 1) {
        auto end = tokens[index].range.end;
        auto next = index + 1 < tokens.size() ? tokens[index + 1].range.begin
                                              : static_cast<std::uint32_t>(content.size());
        if(content.slice(end, next).count('\n') > 1) {
            offset = line_end(content, end);
        }
    }
    return offset;
}

/// Where the line of the `module;` opening the global module fragment
/// ends, the comments it runs on to included, when the file opens with one.
std::optional<std::uint32_t> fragment_end(const LexicalInfo& lexical) {
    if(lexical.modules.empty() ||
       lexical.modules.front().kind != LexicalInfo::ModuleDeclaration::Kind::GlobalFragment) {
        return std::nullopt;
    }
    auto rest = llvm::ArrayRef(lexical.tokens)
                    .drop_front(lexical.token_at(lexical.modules.front().keyword.end));
    return llvm::find_if(rest, [](const Token& token) { return token.is_eod(); })->range.begin;
}

/// Where the code of the file starts: its first token that is neither a
/// comment, a directive's nor of the `module;` opening the global module
/// fragment.
std::uint32_t code_begin(llvm::StringRef content, const LexicalInfo& lexical) {
    auto fragment = fragment_end(lexical).value_or(0);
    auto directive = lexical.directives.begin();
    for(std::uint32_t index = 0; index < lexical.tokens.size(); index += 1) {
        if(directive != lexical.directives.end() && index == directive->first) {
            index = directive->last;
            ++directive;
            continue;
        }
        const auto& token = lexical.tokens[index];
        if(token.kind != clang::tok::comment && !token.is_eod() && token.range.end > fragment) {
            return token.range.begin;
        }
    }
    return static_cast<std::uint32_t>(content.size());
}

}  // namespace

/// After the last `#include` of the file's leading directives — those
/// before its first declaration — at the file's own level: outside every
/// conditional, or directly inside its include guard. An include under
/// `#if FEATURE`, inside `extern "C"` or a type body, or trailing the
/// code (an X-macro list, a `.tpp` body) is no place for one that must
/// always apply. Without one, after the leading `#pragma once`, the
/// guard's `#define` or the `module;` opening the global module fragment,
/// else after the comments opening the file. A module unit without that
/// fragment opens it there: no include may follow the module declaration.
/// Only a guard enclosing the whole file is one (LexicalInfo::IncludeGuard);
/// a leading `#ifndef _GNU_SOURCE` block is not. The text's lexical scan
/// rather than the compile's directives: the preamble's are compiled into
/// the PCH and never reach this AST.
IncludeInsertion include_insertion(CompilationUnit& unit) {
    using Kind = LexicalInfo::Directive::Kind;
    auto focus = unit.focus_file();
    auto content = focus.content();
    const auto& lexical = focus.lexical();

    /// The anchors at one conditional depth: the file's own level, and
    /// the level directly inside the include guard.
    struct Level {
        std::optional<std::uint32_t> include;
        std::optional<std::uint32_t> prologue;
    };

    std::array<Level, 2> levels;
    auto fragment = fragment_end(lexical);
    if(fragment) {
        levels[0].prologue = line_end(content, *fragment);
    }
    auto& guard = lexical.include_guard;
    auto code = code_begin(content, lexical);
    for(auto [index, directive]: llvm::enumerate(lexical.directives)) {
        if(directive.range.begin >= code) {
            break;
        }
        auto anchor = line_end(content, directive.range.end);
        if(guard && index == guard->definition) {
            levels[1].prologue = anchor;
        } else if(directive.depth < levels.size() && directive.kind == Kind::Include) {
            levels[directive.depth].include = anchor;
        } else if(directive.depth < levels.size() && directive.kind == Kind::PragmaOnce) {
            levels[directive.depth].prologue = anchor;
        }
    }
    // A line starting with `module` declares one only in a module unit; a
    // header compiled in its includer's context is none, whatever the
    // includer is.
    bool declares_module = llvm::any_of(lexical.modules, [](const auto& module) {
        return module.kind == LexicalInfo::ModuleDeclaration::Kind::Declaration;
    });
    if(declares_module && !fragment && unit.is_named_module()) {
        return {.offset = after_leading_comments(content, lexical), .opens_fragment = true};
    }
    const auto& level = levels[guard ? 1 : 0];
    if(auto anchor = level.include ? level.include : level.prologue) {
        return {.offset = *anchor};
    }
    return {.offset = after_leading_comments(content, lexical)};
}

std::uint32_t import_insertion(llvm::StringRef content, const clang::LangOptions& options) {
    std::uint32_t anchor = 0;
    bool fragment = false;
    bool declared = false;
    // An import inside a conditional block moves the anchor past the block:
    // the next one would be as conditional.
    std::uint32_t depth = 0;
    bool held = false;
    Lexer lexer(content, options);
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        // The lexer ends a module declaration's line as it ends a directive.
        if(token.is_eod()) {
            continue;
        }
        if(token.is_directive_hash()) {
            using enum LexicalInfo::Directive::Kind;
            auto keyword = lexer.advance();
            auto kind = directive_kind(keyword.is_eod() ? "" : keyword.text(content), "");
            auto next = keyword;
            while(!next.is_eod() && !next.is_eof()) {
                next = lexer.advance();
            }
            auto end = line_end(content, next.range.begin);
            if(kind == If) {
                depth += 1;
            } else if(kind == EndIf && depth > 0) {
                depth -= 1;
                if(depth == 0 && held) {
                    anchor = end;
                    held = false;
                }
            }
            // A module unit's directives before its declaration are in its
            // global module fragment, where no import may go.
            if(!fragment && !declared) {
                anchor = end;
            }
            continue;
        }
        auto text = token.text(content);
        if(text == "export") {
            token = lexer.advance();
            text = token.text(content);
        }
        if(text != "module" && text != "import") {
            break;
        }
        auto next = lexer.advance();
        if(text == "module" && next.kind == clang::tok::semi) {
            fragment = true;
            continue;
        }
        while(next.kind != clang::tok::semi && !next.is_eof()) {
            next = lexer.advance();
        }
        declared |= text == "module";
        if(depth > 0) {
            held = true;
        } else {
            anchor = line_end(content, next.range.end);
        }
    }
    return anchor;
}

void add_include(CompilationUnit& unit, LocalSourceRange selection, std::vector<CodeAction>& out) {
    namespace stdlib = clang::tooling::stdlib;
    auto name = qualified_name_at(unit, selection.begin);
    if(!name) {
        return;
    }
    auto focus = unit.focus_file();
    bool unresolved = llvm::any_of(unit.diagnostics(), [&](const Diagnostic& diagnostic) {
        return focus.contains(diagnostic.fid) && diagnostic.range.valid() &&
               unresolved_name(diagnostic.id.value) && diagnostic.range.intersects(name->range);
    });
    if(!unresolved) {
        return;
    }
    auto insertion = include_insertion(unit);

    auto language = unit.lang_options().CPlusPlus ? stdlib::Lang::CXX : stdlib::Lang::C;
    llvm::SmallVector<llvm::StringRef, 2> scopes;
    if(!name->scope.empty()) {
        scopes.push_back(name->scope);
    } else if(language == stdlib::Lang::CXX) {
        scopes = {"std::", ""};
    } else {
        scopes.push_back("");
    }
    for(auto scope: scopes) {
        std::optional<stdlib::Header> header;
        if(auto symbol = stdlib::Symbol::named(scope, name->name, language)) {
            header = symbol->header();
        } else if(auto overloaded = overloaded_standard_header(scope, *name)) {
            header = overloaded;
        } else {
            continue;
        }
        if(header) {
            out.push_back(CodeAction{
                .title = std::format("Add #include {}", header->name()),
                .kind = protocol::CodeActionKind::QuickFix,
                .edits = {{{insertion.offset, insertion.offset}, insertion.text(header->name())}},
            });
        }
        // A standard library name spelled with its namespace: the index
        // knows no better header, only the library's internal ones.
        if(!name->scope.empty()) {
            return;
        }
        break;
    }
    out.push_back(CodeAction{
        .title = std::format("Add #include for '{}{}'", name->scope, name->name),
        .kind = protocol::CodeActionKind::QuickFix,
        .index = IncludeRequest{.scope = name->scope, .name = name->name, .insertion = insertion},
    });
}

}  // namespace clice::feature::action
