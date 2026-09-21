#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"

namespace clice::feature::action {

void populate_switch(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* stmt = ctx.node.get<clang::SwitchStmt>();
    if(!stmt || !stmt->getCond()) {
        return;
    }
    const auto* body = llvm::dyn_cast_if_present<clang::CompoundStmt>(stmt->getBody());
    if(!body) {
        return;
    }
    auto type = stmt->getCond()->IgnoreImplicit()->getType();
    const auto* enum_type = type->getAs<clang::EnumType>();
    if(!enum_type) {
        return;
    }
    const auto* enum_decl = enum_type->getDecl()->getDefinition();
    if(!enum_decl || enum_decl->isDependentType()) {
        return;
    }

    auto& context = unit.context();
    llvm::DenseSet<std::int64_t> covered;
    const clang::DefaultStmt* default_stmt = nullptr;
    for(const auto* current = stmt->getSwitchCaseList(); current;
        current = current->getNextSwitchCase()) {
        if(auto* default_case = llvm::dyn_cast<clang::DefaultStmt>(current)) {
            default_stmt = default_case;
            continue;
        }
        const auto* case_stmt = llvm::cast<clang::CaseStmt>(current);
        clang::Expr::EvalResult value;
        if(case_stmt->getRHS() || !case_stmt->getLHS()->EvaluateAsInt(value, context)) {
            return;
        }
        covered.insert(value.Val.getInt().getExtValue());
    }

    llvm::SmallVector<const clang::EnumConstantDecl*> missing;
    for(const auto* enumerator: enum_decl->enumerators()) {
        if(covered.insert(enumerator->getInitVal().getExtValue()).second) {
            missing.push_back(enumerator);
        }
    }
    if(missing.empty()) {
        return;
    }

    auto content = unit.main_content();
    std::optional<LocalSourceRange> anchor;
    std::string indent;
    if(default_stmt) {
        anchor = main_range(unit, default_stmt->getDefaultLoc());
    } else {
        anchor = main_range(unit, body->getRBracLoc());
    }
    if(!anchor) {
        return;
    }
    if(const auto* first = stmt->getSwitchCaseList(); first && !default_stmt) {
        if(auto range = main_range(unit, first->getKeywordLoc())) {
            indent = line_indent(content, range->begin).str();
        }
    }
    if(default_stmt) {
        indent = line_indent(content, anchor->begin).str();
    } else if(indent.empty()) {
        indent = line_indent(content, main_range(unit, stmt->getSwitchLoc())->begin).str() + "    ";
    }

    auto begin = line_begin(content, anchor->begin);
    bool own_line = content.substr(begin, anchor->begin - begin).trim().empty();
    const auto& from = ctx.node.decl_context();
    std::string text = own_line ? "" : "\n";
    for(const auto* enumerator: missing) {
        text += std::format("{}case {}:\n", indent, name_at(enumerator, &from));
    }
    if(!default_stmt) {
        text += std::format("{}    break;\n", indent);
    }
    if(!own_line) {
        text += indent;
    }
    auto offset = own_line ? begin : anchor->begin;
    out.push_back(CodeAction{
        .id = "populate-switch",
        .title = std::format("Add {} missing enum case{} to switch",
                             missing.size(),
                             missing.size() == 1 ? "" : "s"),
        .kind = CodeActionKind::RefactorRewrite,
        .edits = {{{offset, offset}, std::move(text)}},
    });
}

}  // namespace clice::feature::action
