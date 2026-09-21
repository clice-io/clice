#include <algorithm>
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

/// After the main file's last `#include` line among the ones nested
/// least deeply in conditionals — an include inside `#if FEATURE` is no
/// place for one that must always apply, while an include guard wraps
/// them all — else after its `#pragma once`, else after the guard's
/// `#define`, else at its start. A raw lex of the text rather than the
/// directive table: the preamble's directives are compiled into the PCH
/// and never reach this AST.
std::uint32_t include_insertion_offset(CompilationUnitRef unit) {
    auto content = unit.main_content();
    std::optional<std::uint32_t> last_include;
    std::uint32_t include_depth = 0;
    std::optional<std::uint32_t> pragma_once;
    std::optional<std::uint32_t> guard_define;
    std::optional<llvm::StringRef> guard_macro;
    std::uint32_t depth = 0;
    std::uint32_t directives = 0;
    Lexer lexer(content, {.lang_opts = &unit.lang_options()});
    for(auto token = lexer.advance(); !token.is_eof(); token = lexer.advance()) {
        if(!token.is_directive_hash()) {
            continue;
        }
        auto keyword = lexer.advance();
        if(!keyword.is_identifier()) {
            continue;
        }
        directives += 1;
        auto text = keyword.text(content);
        if(text == "include") {
            if(!last_include || depth <= include_depth) {
                last_include = token.range.begin;
                include_depth = depth;
            }
        } else if(text == "pragma") {
            if(lexer.advance().text(content) == "once") {
                pragma_once = token.range.begin;
            }
        } else if(text == "if" || text == "ifdef" || text == "ifndef") {
            if(text == "ifndef" && directives == 1) {
                guard_macro = lexer.advance().text(content);
            }
            depth += 1;
        } else if(text == "define") {
            if(guard_macro && directives == 2 && lexer.advance().text(content) == *guard_macro) {
                guard_define = token.range.begin;
            }
        } else if(text == "endif" && depth > 0) {
            depth -= 1;
        }
    }
    for(auto anchor: {last_include, pragma_once, guard_define}) {
        if(anchor) {
            return line_end(content, *anchor);
        }
    }
    return 0;
}

}  // namespace

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
    auto content = unit.main_content();
    auto offset = include_insertion_offset(unit);
    // A last line without its newline: the directive still needs a line
    // of its own.
    std::string before = offset == content.size() && !content.ends_with('\n') ? "\n" : "";

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
                .edits = {{{offset, offset},
                           std::format("{}#include {}\n", before, header.name())}},
            });
        }
        break;
    }
    out.push_back(CodeAction{
        .title = std::format("Add #include for '{}{}'", name->scope, name->name),
        .kind = protocol::CodeActionKind::quick_fix,
        .index = IncludeRequest{.scope = name->scope, .name = name->name, .offset = offset},
    });
}

}  // namespace clice::feature::action
