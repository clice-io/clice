#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/types.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/TypeLoc.h"

namespace clice::feature::action {

void expand_deduced_type(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* loc = ctx.node.get<clang::TypeLoc>();
    if(!loc) {
        return;
    }
    auto inner = types::unwrap(*loc);
    std::optional<clang::QualType> deduced;
    clang::SourceRange written;
    if(auto auto_loc = inner.getAs<clang::AutoTypeLoc>()) {
        if(auto_loc.isDecltypeAuto()) {
            return;
        }
        written = auto_loc.getLocalSourceRange();
        deduced = types::deduced_type(unit.context(), auto_loc.getNameLoc());
    } else if(auto decltype_loc = inner.getAs<clang::DecltypeTypeLoc>()) {
        written = decltype_loc.getLocalSourceRange();
        deduced = decltype_loc.getTypePtr()->getUnderlyingType();
    } else {
        return;
    }
    if(!deduced || deduced->isNull() || (*deduced)->isDependentType()) {
        return;
    }
    auto range = main_range(unit, written);
    if(!range) {
        return;
    }
    auto printed = type_name(unit.context(), *deduced, &ctx.node.decl_context());
    llvm::StringRef view = printed;
    if(view.contains("(lambda") || view.contains("(anonymous") || view.contains("(unnamed")) {
        return;
    }
    auto content = unit.main_content();
    out.push_back(CodeAction{
        .id = "expand-deduced-type",
        .title = std::format("Replace '{}' with '{}'",
                             content.substr(range->begin, range->length()),
                             printed),
        .kind = CodeActionKind::RefactorRewrite,
        .edits = {{*range, std::move(printed)}},
    });
}

}  // namespace clice::feature::action
