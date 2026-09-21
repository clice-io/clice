#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/types.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/TypeLoc.h"

namespace clice::feature::action {

void expand_deduced_type(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* loc = ctx.node.get<clang::TypeLoc>();
    if(!loc) {
        return;
    }
    // A structured binding's declared type must stay `auto`.
    for(const auto* node = ctx.node.parent; node; node = node->parent) {
        if(const auto* decl = node->get<clang::Decl>()) {
            if(llvm::isa<clang::DecompositionDecl>(decl)) {
                return;
            }
            break;
        }
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
    // `auto&&` bound to an lvalue deduces a reference: the written `&&`
    // goes with the `auto`, or the result would be a reference to a
    // reference.
    if((*deduced)->isReferenceType()) {
        const auto* parent = ctx.node.parent ? ctx.node.parent->get<clang::TypeLoc>() : nullptr;
        auto reference = parent ? parent->getAs<clang::RValueReferenceTypeLoc>()
                                : clang::RValueReferenceTypeLoc();
        if(!reference) {
            return;
        }
        written.setEnd(reference.getSigilLoc());
    }
    auto range = main_range(unit, written);
    if(!range) {
        return;
    }
    // Only a type spelled before its declarator can stand in for `auto`:
    // `int (*)(int)` has nowhere to put the name.
    auto declaration = type_name(unit.context(), *deduced, &ctx.node.decl_context(), "x");
    if(!declaration || !llvm::StringRef(*declaration).ends_with(" x")) {
        return;
    }
    auto printed = declaration->substr(0, declaration->size() - 2);
    auto content = unit.main_content();
    out.push_back(CodeAction{
        .title = std::format("Replace '{}' with '{}'",
                             content.substr(range->begin, range->length()),
                             printed),
        .kind = protocol::CodeActionKind::refactor_rewrite,
        .edits = {{*range, std::move(printed)}},
    });
}

}  // namespace clice::feature::action
