#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"

namespace clice::feature::action {

namespace {

/// Whether two adjacent expanded tokens are written with a space between
/// them: none inside brackets or around member and scope operators, one
/// everywhere else.
bool needs_space(clang::tok::TokenKind left, clang::tok::TokenKind right) {
    using namespace clang::tok;
    switch(right) {
        case r_paren:
        case r_square:
        case comma:
        case semi:
        case period:
        case arrow:
        case coloncolon: return false;
        default: break;
    }
    switch(left) {
        case l_paren:
        case l_square:
        case period:
        case arrow:
        case coloncolon: return false;
        default: return true;
    }
}

}  // namespace

void expand_macro(CompilationUnitRef unit,
                  LocalSourceRange selection,
                  std::vector<CodeAction>& out) {
    auto touching =
        unit.spelled_tokens_touch(unit.create_location(unit.main_file(), selection.begin));
    if(touching.empty()) {
        return;
    }
    auto& SM = unit.context().getSourceManager();
    for(const auto& expansion: unit.expansions_overlapping(touching)) {
        // Directives are mappings too, from their `#`.
        if(expansion.Spelled.empty() ||
           expansion.Spelled.front().kind() != clang::tok::identifier) {
            continue;
        }
        const auto& name = expansion.Spelled.front();
        std::string text;
        const clang::syntax::Token* previous = nullptr;
        for(const auto& token: expansion.Expanded) {
            if(previous && needs_space(previous->kind(), token.kind())) {
                text += ' ';
            }
            text += token.text(SM);
            previous = &token;
        }
        out.push_back(CodeAction{
            .id = "expand-macro",
            .title = std::format("Expand macro '{}'", name.text(SM)),
            .kind = CodeActionKind::RefactorInline,
            .edits = {{{unit.file_offset(name.location()),
                        unit.file_offset(expansion.Spelled.back().endLocation())},
                       std::move(text)}},
        });
        return;
    }
}

}  // namespace clice::feature::action
