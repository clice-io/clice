module clice;

import :compile.compilation;
import :feature.code_actions.action;

namespace clice::feature::action {

namespace {

/// Which of a class's implicit special members are deleted.
struct Deleted {
    bool default_constructor = false;
    bool copy_constructor = false;
    bool move_constructor = false;
    bool copy_assignment = false;
    bool move_assignment = false;
};

/// Clang declares the implicit members on first need, and in a template
/// only in its instantiations: there each one spelled `= default` is
/// deleted per instantiation as the implicit one would be, but the copy
/// members a user-declared move deletes are deleted in all of them.
Deleted deleted_members(CompilationUnit& unit, const clang::CXXRecordDecl* record) {
    if(record->isDependentContext()) {
        bool deletes_copies = record->hasUserDeclaredMoveOperation();
        return {.copy_constructor = deletes_copies, .copy_assignment = deletes_copies};
    }
    unit.sema().ForceDeclarationOfImplicitMembers(const_cast<clang::CXXRecordDecl*>(record));
    Deleted deleted;
    for(const auto* method: record->methods()) {
        if(!method->isImplicit() || !method->isDeleted()) {
            continue;
        }
        if(const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(method)) {
            deleted.default_constructor |= constructor->isDefaultConstructor();
            deleted.copy_constructor |= constructor->isCopyConstructor();
            deleted.move_constructor |= constructor->isMoveConstructor();
        } else {
            deleted.copy_assignment |= method->isCopyAssignmentOperator();
            deleted.move_assignment |= method->isMoveAssignmentOperator();
        }
    }
    return deleted;
}

}  // namespace

void special_members(const Context& ctx, std::vector<CodeAction>& out) {
    auto& unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record->isThisDeclarationADefinition() || record->isInvalidDecl() ||
       record->isInterface() || record->getName().empty() || !unit.lang_options().CPlusPlus11) {
        return;
    }
    auto deleted = deleted_members(unit, record);
    // LIMITATION: in a class template, clang counts a copy or move the class
    // declares only when its parameter is spelled as the class itself, and
    // tells the copies' `const` only from the members whose types are known:
    // a copy spelled through a dependent type (`std::pair`'s conditional
    // assignment) or followed by a parameter pack is declared again, and an
    // instantiation whose member copies only from a non-const object (as
    // `std::auto_ptr` does) loses its copies to the `const` ones declared here.
    // A move that a user-declared copy member or destructor suppresses stays
    // undeclared: `= delete` would reject the rvalues that copy now, and
    // `= default` would move them instead of calling the user's copy.
    bool moves = !record->hasUserDeclaredCopyConstructor() &&
                 !record->hasUserDeclaredCopyAssignment() && !record->hasUserDeclaredDestructor() &&
                 !record->hasUserDeclaredMoveOperation();

    auto name = record->getName();
    std::vector<Member> members;
    auto add = [&](std::string declarator, bool is_deleted) {
        members.push_back({
            clang::AS_public,
            std::format("{} = {};", declarator, is_deleted ? "delete" : "default"),
        });
    };
    // The copy constructor declared below suppresses the implicit default one.
    if(!record->hasUserDeclaredConstructor()) {
        add(std::format("{}()", name), deleted.default_constructor);
    }
    if(!record->hasUserDeclaredCopyConstructor()) {
        auto qualifier = record->implicitCopyConstructorHasConstParam() ? "const " : "";
        add(std::format("{0}({1}{0}&)", name, qualifier), deleted.copy_constructor);
    }
    // A deleted implicit move takes no part in overload resolution, so an
    // rvalue copies; once the copy members are declared, an undeclared move
    // keeps that, where a move declared deleted would be chosen and fail.
    if(moves && !deleted.move_constructor) {
        add(std::format("{0}({0}&&)", name), false);
    }
    if(!record->hasUserDeclaredCopyAssignment()) {
        auto qualifier = record->implicitCopyAssignmentHasConstParam() ? "const " : "";
        add(std::format("{0}& operator=({1}{0}&)", name, qualifier), deleted.copy_assignment);
    }
    if(moves && !deleted.move_assignment) {
        add(std::format("{0}& operator=({0}&&)", name), false);
    }
    if(members.empty()) {
        return;
    }
    auto insertion = insert_members(unit, record, members);
    if(!insertion) {
        return;
    }
    out.push_back(CodeAction{
        .title = std::format("Declare the implicit special members of '{}'", name),
        .kind = action_kind::declare_special_members,
        .edits = {std::move(*insertion)},
    });
}

}  // namespace clice::feature::action
