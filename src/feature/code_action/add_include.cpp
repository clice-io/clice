#include <algorithm>
#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

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
    auto it = std::ranges::partition_point(tokens, [&](const clang::syntax::Token& token) {
        return offset_of(token) + token.length() <= offset;
    });
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

/// After the main file's last `#include` line, else after its `#pragma
/// once`, else at its start. A line scan rather than the directive table:
/// the preamble's directives are compiled into the PCH and never reach
/// this AST.
std::uint32_t include_insertion_offset(CompilationUnitRef unit) {
    auto content = unit.main_content();
    std::optional<std::uint32_t> last_include;
    std::optional<std::uint32_t> pragma_once;
    std::uint32_t offset = 0;
    for(llvm::StringRef line: llvm::split(content, '\n')) {
        auto directive = line.ltrim(" \t");
        if(directive.consume_front("#")) {
            directive = directive.ltrim(" \t");
            if(directive.starts_with("include")) {
                last_include = offset;
            } else if(directive.starts_with("pragma once")) {
                pragma_once = offset;
            }
        }
        offset += static_cast<std::uint32_t>(line.size() + 1);
    }
    if(last_include) {
        return line_end(content, *last_include);
    }
    if(pragma_once) {
        return line_end(content, *pragma_once);
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
    auto offset = include_insertion_offset(unit);

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
                .id = "add-include",
                .title = std::format("Add #include {}", header.name()),
                .kind = CodeActionKind::QuickFix,
                .edits = {{{offset, offset}, std::format("#include {}\n", header.name())}},
            });
        }
        break;
    }
    out.push_back(CodeAction{
        .id = "add-include",
        .title = std::format("Add #include for '{}{}'", name->scope, name->name),
        .kind = CodeActionKind::QuickFix,
        .index = IncludeRequest{.scope = name->scope, .name = name->name, .offset = offset},
    });
}

}  // namespace clice::feature::action
