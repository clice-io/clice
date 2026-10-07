module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :feature.code_action.action;
import :semantic.display;

namespace clice::feature::action {

/// Whether a switch body declares something at its own scope, which a
/// label added after the declaration would jump past.
static bool declares_in_scope(const clang::CompoundStmt* body) {
    return llvm::any_of(body->body(), [](const clang::Stmt* statement) {
        return llvm::isa<clang::DeclStmt>(statement->stripLabelLikeStatements());
    });
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
    // end, or at the top of the body when it declares something a label
    // would jump past — nothing falls into them there, and no conditional
    // directive around the first label takes them along. Either way on
    // the labels' own indentation.
    auto content = unit.main_content();
    auto anchor_loc = body->getRBracLoc();
    if(default_stmt) {
        anchor_loc = default_stmt->getKeywordLoc();
    } else if(declares_in_scope(body)) {
        auto brace = main_range(unit, body->getLBracLoc());
        if(!brace) {
            return;
        }
        auto tokens = unit.spelled_tokens(unit.main_file());
        auto after = std::ranges::partition_point(tokens, [&](const clang::syntax::Token& token) {
            return unit.file_offset(token.location()) <= brace->begin;
        });
        anchor_loc = after->location();
    }
    auto anchor = main_range(unit, anchor_loc);
    if(!anchor) {
        return;
    }
    std::string indent;
    if(const auto* label = stmt->getSwitchCaseList()) {
        auto range = main_range(unit, label->getKeywordLoc());
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
        .kind = protocol::CodeActionKind::RefactorRewrite,
        .edits = {{{offset, offset}, std::move(text)}},
    });
}

}  // namespace clice::feature::action
