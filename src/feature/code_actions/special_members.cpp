module clice;

import :compile.compilation;
import :feature.code_actions.action;

namespace clice::feature::action {

/// The line declaring the special member `method`, its declarator spelled as
/// `declarator`: defaulted when clang would define it, deleted when it deletes
/// or does not declare it; nothing when the user declared it already.
static std::optional<std::string> declaration_line(const clang::CXXMethodDecl* method,
                                                   llvm::StringRef declarator) {
    if(method && !method->isImplicit()) {
        return std::nullopt;
    }
    return std::format("{} = {};",
                       declarator,
                       method && !method->isDeleted() ? "default" : "delete");
}

void special_members(const Context& ctx, std::vector<CodeAction>& out) {
    auto& unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record || !record->isThisDeclarationADefinition() || record->isInvalidDecl() ||
       record->isUnion() || record->isLambda() || record->getName().empty() ||
       !unit.lang_options().CPlusPlus11) {
        return;
    }
    bool needs_copy =
        !record->hasUserDeclaredCopyConstructor() || !record->hasUserDeclaredCopyAssignment();
    bool needs_move =
        !record->hasUserDeclaredMoveConstructor() || !record->hasUserDeclaredMoveAssignment();
    if(!needs_copy && !needs_move) {
        return;
    }
    // Clang declares the implicit members on first need; Sema takes the
    // class non-const to declare them here.
    unit.sema().ForceDeclarationOfImplicitMembers(const_cast<clang::CXXRecordDecl*>(record));

    const clang::CXXConstructorDecl* copy_constructor = nullptr;
    const clang::CXXConstructorDecl* move_constructor = nullptr;
    const clang::CXXMethodDecl* copy_assignment = nullptr;
    const clang::CXXMethodDecl* move_assignment = nullptr;
    for(const auto* method: record->methods()) {
        unsigned qualifiers = 0;
        if(const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
            if(constructor->isCopyConstructor(qualifiers)) {
                copy_constructor = constructor;
            } else if(constructor->isMoveConstructor(qualifiers)) {
                move_constructor = constructor;
            }
        } else if(method->isCopyAssignmentOperator()) {
            copy_assignment = method;
        } else if(method->isMoveAssignmentOperator()) {
            move_assignment = method;
        }
    }

    auto name = record->getName();
    std::vector<Member> members;
    auto add = [&](const clang::CXXMethodDecl* method, llvm::StringRef declarator) {
        if(auto line = declaration_line(method, declarator)) {
            members.push_back({clang::AS_public, std::move(*line)});
        }
    };
    add(copy_constructor, std::format("{}(const {}&)", name, name));
    add(move_constructor, std::format("{}({}&&)", name, name));
    add(copy_assignment, std::format("{}& operator=(const {}&)", name, name));
    add(move_assignment, std::format("{}& operator=({}&&)", name, name));
    auto insertion = insert_members(unit, record, members);
    if(!insertion) {
        return;
    }
    llvm::StringRef copy_or_move = needs_copy ? (needs_move ? "copy/move" : "copy") : "move";
    out.push_back(CodeAction{
        .title = std::format("Declare the implicit {} members of '{}'", copy_or_move, name),
        .kind = action_kind::declare_special_members,
        .edits = {std::move(*insertion)},
    });
}

}  // namespace clice::feature::action
