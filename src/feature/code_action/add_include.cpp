#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "syntax/lexer.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Inclusions/StandardLibrary.h"

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
    /// The chain's span in the main file.
    LocalSourceRange range;
};

/// None for a member named through `.` or `->`: the object's class
/// declares it, not a header.
std::optional<QualifiedName> qualified_name_at(CompilationUnitRef unit, std::uint32_t offset) {
    auto tokens = unit.spelled_tokens(unit.main_file());
    auto& SM = unit.context().getSourceManager();
    auto offset_of = [&](const clang::syntax::Token& token) {
        return unit.file_offset(token.location());
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
    return result;
}

/// Where the text the user edits ends: a header compiled in its
/// includer's context carries one line more, the include entering the
/// synthesized rest of the includer.
std::uint32_t text_end(CompilationUnitRef unit) {
    auto content = unit.main_content();
    auto it = unit.directives().find(unit.main_file());
    if(it != unit.directives().end() && !it->second.includes.empty()) {
        const auto& last = it->second.includes.back();
        if(last.fid.isValid() && unit.synthesized(last.fid)) {
            return line_begin(content, unit.file_offset(last.location));
        }
    }
    return content.size();
}

}  // namespace

/// After the last `#include` of the file's leading directives — those
/// before its first declaration — at the file's own level: outside every
/// conditional, or directly inside its include guard. An include under
/// `#if FEATURE`, inside `extern "C"` or a type body, or trailing the
/// code (an X-macro list, a `.tpp` body) is no place for one that must
/// always apply. Without one, after the leading `#pragma once`, the
/// guard's `#define` or the `module;` opening the global module fragment,
/// else at the file's start. A module unit without that fragment opens
/// it at the start: no include may follow the module declaration. Only an `#ifndef`/`#define` pair
/// enclosing the whole file is a guard; a leading `#ifndef _GNU_SOURCE` block is not. A raw lex of
/// the text rather than the directive table: the preamble's directives are compiled into the PCH
/// and never reach this AST.
IncludeInsertion include_insertion(CompilationUnitRef unit) {
    auto content = unit.main_content();
    auto end = text_end(unit);

    /// The anchors at one conditional depth: the file's own level, and
    /// the level directly inside the include guard.
    struct Level {
        std::optional<std::uint32_t> include;
        std::optional<std::uint32_t> prologue;
    };

    std::array<Level, 2> levels;
    enum class Guard : std::uint8_t { None, Opened, Defined, Closed };
    auto guard = Guard::None;
    llvm::StringRef guard_macro;
    bool seen_code = false;
    bool fragment = false;
    bool declares_module = false;
    std::uint32_t depth = 0;
    std::uint32_t directives = 0;
    Lexer lexer(content, {.lang_opts = &unit.lang_options()});
    for(auto token = lexer.advance(); !token.is_eof() && token.range.begin < end;
        token = lexer.advance()) {
        if(guard == Guard::Closed) {
            guard = Guard::None;
        }
        bool module_line = token.is_pp_keyword && token.text(content) == "module";
        if(!token.is_directive_hash() && !module_line) {
            if(guard == Guard::Opened) {
                guard = Guard::None;
            }
            seen_code = true;
            continue;
        }
        llvm::SmallVector<Token, 4> line;
        auto next = lexer.advance();
        for(; !next.is_eod() && !next.is_eof(); next = lexer.advance()) {
            line.push_back(next);
        }
        auto anchor = line_end(content, next.range.begin);
        if(module_line) {
            if(!seen_code && line.size() == 1 && line[0].kind == clang::tok::semi) {
                levels[0].prologue = anchor;
                fragment = true;
            } else {
                declares_module = true;
                seen_code = true;
            }
            continue;
        }
        directives += 1;
        if(line.empty() || !line[0].is_identifier()) {
            continue;
        }
        auto keyword = line[0].text(content);
        auto argument = line.size() > 1 ? line[1].text(content) : llvm::StringRef();
        if(guard == Guard::Opened) {
            if(keyword == "define" && argument == guard_macro) {
                guard = Guard::Defined;
                levels[1].prologue = anchor;
                continue;
            }
            guard = Guard::None;
        }
        if(keyword == "include") {
            if(!seen_code && depth < levels.size()) {
                levels[depth].include = anchor;
            }
        } else if(keyword == "pragma") {
            if(!seen_code && depth < levels.size() && argument == "once") {
                levels[depth].prologue = anchor;
            }
        } else if(keyword == "if" || keyword == "ifdef" || keyword == "ifndef") {
            if(keyword == "ifndef" && directives == 1 && !seen_code) {
                guard = Guard::Opened;
                guard_macro = argument;
            }
            depth += 1;
        } else if(keyword == "elif" || keyword == "elifdef" || keyword == "elifndef" ||
                  keyword == "else") {
            if(depth == 1 && guard == Guard::Defined) {
                guard = Guard::None;
            }
        } else if(keyword == "endif" && depth > 0) {
            depth -= 1;
            if(depth == 0 && guard == Guard::Defined) {
                guard = Guard::Closed;
            }
        }
    }
    if(declares_module && !fragment) {
        return {.offset = 0, .opens_fragment = true};
    }
    const auto& level = levels[guard == Guard::Closed ? 1 : 0];
    return {.offset = level.include.value_or(level.prologue.value_or(0))};
}

void add_include(CompilationUnitRef unit,
                 LocalSourceRange selection,
                 std::vector<CodeAction>& out) {
    namespace stdlib = clang::tooling::stdlib;
    auto name = qualified_name_at(unit, selection.begin);
    if(!name) {
        return;
    }
    auto main = unit.main_file();
    bool unresolved = llvm::any_of(unit.diagnostics(), [&](const Diagnostic& diagnostic) {
        return diagnostic.fid == main && diagnostic.range.valid() &&
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
        auto symbol = stdlib::Symbol::named(scope, name->name, language);
        if(!symbol) {
            continue;
        }
        for(auto header: symbol->headers()) {
            out.push_back(CodeAction{
                .title = std::format("Add #include {}", header.name()),
                .kind = protocol::CodeActionKind::quick_fix,
                .edits = {{{insertion.offset, insertion.offset}, insertion.text(header.name())}},
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
        .kind = protocol::CodeActionKind::quick_fix,
        .index = IncludeRequest{.scope = name->scope, .name = name->name, .insertion = insertion},
    });
}

}  // namespace clice::feature::action
