#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/display.h"

#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"

namespace clice::feature::action {

/// The first label of a switch body that declares something at its own
/// scope past that label, which a label added after the declaration would
/// jump past; null for a body declaring nothing there.
const static clang::SwitchCase* first_label_if_declaring(const clang::CompoundStmt* body) {
    const clang::SwitchCase* first = nullptr;
    for(const clang::Stmt* statement: body->body()) {
        if(const auto* label = llvm::dyn_cast<clang::SwitchCase>(statement)) {
            first = first ? first : label;
            while(const auto* nested = llvm::dyn_cast<clang::SwitchCase>(statement)) {
                statement = nested->getSubStmt();
            }
        }
        if(first && llvm::isa<clang::DeclStmt>(statement)) {
            return first;
        }
    }
    return nullptr;
}

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

    // Case values are converted to the promoted condition type; the
    // enumerators are brought into it to compare.
    auto& context = unit.context();
    auto condition = stmt->getCond()->getType();
    auto promoted = [&](llvm::APSInt value) {
        value = value.extOrTrunc(context.getIntWidth(condition));
        value.setIsUnsigned(condition->isUnsignedIntegerOrEnumerationType());
        return value;
    };
    llvm::DenseSet<llvm::APSInt> covered;
    const clang::DefaultStmt* default_stmt = nullptr;
    for(const auto* current = stmt->getSwitchCaseList(); current;
        current = current->getNextSwitchCase()) {
        if(auto* default_case = llvm::dyn_cast<clang::DefaultStmt>(current)) {
            default_stmt = default_case;
            continue;
        }
        // A label depending on template parameters covers enumerators
        // only an instantiation knows.
        const auto* case_stmt = llvm::cast<clang::CaseStmt>(current);
        clang::Expr::EvalResult value;
        if(case_stmt->getRHS() || case_stmt->getLHS()->isValueDependent() ||
           !case_stmt->getLHS()->EvaluateAsInt(value, context)) {
            return;
        }
        covered.insert(promoted(value.Val.getInt()));
    }

    llvm::SmallVector<const clang::EnumConstantDecl*> missing;
    for(const auto* enumerator: enum_decl->enumerators()) {
        if(covered.insert(promoted(enumerator->getInitVal())).second) {
            missing.push_back(enumerator);
        }
    }
    if(missing.empty()) {
        return;
    }

    // The labels go before `default`, falling through into it as the
    // missing cases already did; else with a `break` of their own at the
    // end, or before the first label when a declaration lies in between,
    // where nothing falls into them. Either way on the labels' own
    // indentation.
    auto content = unit.main_content();
    const clang::SwitchCase* next = default_stmt ? default_stmt : first_label_if_declaring(body);
    auto anchor = main_range(unit, next ? next->getKeywordLoc() : body->getRBracLoc());
    if(!anchor) {
        return;
    }
    std::string indent;
    if(next) {
        indent = line_indent(content, anchor->begin).str();
    } else if(const auto* first = stmt->getSwitchCaseList()) {
        auto range = main_range(unit, first->getKeywordLoc());
        if(!range) {
            return;
        }
        indent = line_indent(content, range->begin).str();
    } else {
        auto range = main_range(unit, stmt->getSwitchLoc());
        if(!range) {
            return;
        }
        indent = line_indent(content, range->begin).str() + "    ";
    }

    auto begin = line_begin(content, anchor->begin);
    bool own_line = content.substr(begin, anchor->begin - begin).trim().empty();
    const auto& from = ctx.node.decl_context();
    std::string text = own_line ? "" : "\n";
    for(const auto* enumerator: missing) {
        text += std::format("{}case {}{}:\n",
                            indent,
                            qualifier_at(enumerator->getDeclContext(), &from),
                            display::name_of(enumerator, {.qualified = false}));
    }
    if(!default_stmt) {
        text += std::format("{}    break;\n", indent);
    }
    if(!own_line) {
        text += indent;
    }
    auto offset = own_line ? begin : anchor->begin;
    out.push_back(CodeAction{
        .title = std::format("Add {} missing enum case{} to switch",
                             missing.size(),
                             missing.size() == 1 ? "" : "s"),
        .kind = protocol::CodeActionKind::refactor_rewrite,
        .edits = {{{offset, offset}, std::move(text)}},
    });
}

}  // namespace clice::feature::action
