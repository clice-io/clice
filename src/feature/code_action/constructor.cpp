#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"

namespace clice::feature::action {

namespace {

/// Whether a derived class's constructor can leave `record` to its
/// default constructor: one it declares must be neither deleted nor
/// private; the implicit one exists only without user-declared
/// constructors, and is deleted by a reference member or a const one
/// that is not const-default-constructible without an initializer, or
/// by a base or member that cannot default-construct itself.
bool default_constructible(const clang::CXXRecordDecl* record) {
    record = record->getDefinition();
    if(!record || record->isInvalidDecl()) {
        return false;
    }
    for(const auto* ctor: record->ctors()) {
        if(ctor->isDefaultConstructor()) {
            return !ctor->isDeleted() && ctor->getAccess() != clang::AS_private;
        }
    }
    if(record->hasUserDeclaredConstructor()) {
        return false;
    }
    for(const auto& base: record->bases()) {
        const auto* base_record = base.getType()->getAsCXXRecordDecl();
        if(!base_record || !default_constructible(base_record)) {
            return false;
        }
    }
    for(const auto* field: record->fields()) {
        if(field->hasInClassInitializer()) {
            continue;
        }
        auto type = field->getASTContext().getBaseElementType(field->getType());
        if(type->isReferenceType()) {
            return false;
        }
        const auto* member = type->getAsCXXRecordDecl();
        if(member && !default_constructible(member)) {
            return false;
        }
        if(type.isConstQualified() && (!member || !member->allowConstDefaultInit())) {
            return false;
        }
    }
    return true;
}

/// Whether the constructor of `caller` may call `ctor`: a public one, or
/// any its class grants `caller` friendship to, as a class or as a class
/// template (`template <class> friend struct Holder;`).
bool callable(const clang::CXXConstructorDecl* ctor, const clang::CXXRecordDecl* caller) {
    if(ctor->isDeleted()) {
        return false;
    }
    if(ctor->getAccess() == clang::AS_public) {
        return true;
    }
    return llvm::any_of(ctor->getParent()->friends(), [&](const clang::FriendDecl* friend_decl) {
        const clang::CXXRecordDecl* befriended = nullptr;
        if(const auto* type = friend_decl->getFriendType()) {
            befriended = type->getType()->getAsCXXRecordDecl();
        } else if(const auto* pattern = llvm::dyn_cast_if_present<clang::ClassTemplateDecl>(
                      friend_decl->getFriendDecl())) {
            befriended = pattern->getTemplatedDecl();
        }
        return befriended && befriended->getCanonicalDecl() == caller->getCanonicalDecl();
    });
}

/// Whether a `const T&` argument copy-constructs the class. One not yet
/// declared is the implicit copy constructor, deleted per the class's
/// flags or by a user-declared move operation; one whose constraints fail
/// is no candidate.
bool copy_constructible(const clang::CXXRecordDecl* record, const clang::CXXRecordDecl* caller) {
    for(const auto* ctor: record->ctors()) {
        unsigned qualifiers = 0;
        if(!ctor->isIneligibleOrNotSelected() && ctor->isCopyConstructor(qualifiers) &&
           (qualifiers & clang::Qualifiers::Const)) {
            return callable(ctor, caller);
        }
    }
    return record->hasSimpleCopyConstructor() && !record->hasUserDeclaredMoveOperation();
}

/// Whether an rvalue of the class constructs it through a constructor
/// `caller` may call: its move constructor, or a constructor template
/// taking the class by rvalue reference, as MSVC's standard library writes
/// a constrained one. A template counts when nothing keeps it out of
/// overload resolution that can be told without instantiating it: a
/// constraint, or a parameter with neither a default nor a deduction.
bool move_constructible(const clang::CXXRecordDecl* record, const clang::CXXRecordDecl* caller) {
    for(const auto* ctor: record->ctors()) {
        if(!ctor->isIneligibleOrNotSelected() && ctor->isMoveConstructor()) {
            return callable(ctor, caller);
        }
    }
    auto& context = record->getASTContext();
    auto self = context.getCanonicalTagType(record);
    for(const auto* decl: record->decls()) {
        const auto* pattern = llvm::dyn_cast<clang::FunctionTemplateDecl>(decl);
        const auto* ctor =
            pattern ? llvm::dyn_cast<clang::CXXConstructorDecl>(pattern->getTemplatedDecl())
                    : nullptr;
        if(!ctor || ctor->getNumParams() == 0 || ctor->getMinRequiredArguments() > 1) {
            continue;
        }
        auto parameter = ctor->getParamDecl(0)->getType();
        const auto* parameters = pattern->getTemplateParameters();
        if(parameter->isRValueReferenceType() &&
           context.hasSameUnqualifiedType(parameter.getNonReferenceType(), self) &&
           !parameters->getRequiresClause() && !ctor->getTrailingRequiresClause() &&
           parameters->getMinRequiredArguments() == 0) {
            return callable(ctor, caller);
        }
    }
    return record->hasSimpleMoveConstructor();
}

/// Whether `std::move` is declared before `location`.
bool declares_std_move(clang::ASTContext& context, clang::SourceLocation location) {
    auto& SM = context.getSourceManager();
    for(auto* decl: context.getTranslationUnitDecl()->lookup(&context.Idents.get("std"))) {
        if(auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(decl)) {
            return llvm::any_of(ns->lookup(&context.Idents.get("move")), [&](const auto* move) {
                return SM.isBeforeInTranslationUnit(move->getLocation(), location);
            });
        }
    }
    return false;
}

}  // namespace

void memberwise_constructor(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record || !record->isThisDeclarationADefinition() || record->isInvalidDecl() ||
       record->isUnion() || record->isLambda() || record->getName().empty()) {
        return;
    }
    // A base without a usable default constructor would need its own
    // initializer; the constructor initializes the fields alone.
    for(const auto& base: record->bases()) {
        const auto* base_record = base.getType()->getAsCXXRecordDecl();
        if(!base_record || !default_constructible(base_record)) {
            return;
        }
    }

    std::vector<const clang::FieldDecl*> fields;
    for(const auto* field: record->fields()) {
        if(field->getName().empty() || field->getType()->isArrayType()) {
            return;
        }
        fields.push_back(field);
    }
    if(fields.empty()) {
        return;
    }
    for(const auto* ctor: record->ctors()) {
        if(!ctor->isImplicit() && ctor->getNumParams() == fields.size()) {
            return;
        }
    }

    auto& context = unit.context();
    std::string parameters;
    std::string initializers;
    bool moves_any = false;
    for(auto [index, field]: llvm::enumerate(fields)) {
        auto type = field->getType();
        bool moves = type->isRValueReferenceType();
        if(!type->isReferenceType()) {
            type = type.getUnqualifiedType();
            const auto* tag =
                llvm::dyn_cast_if_present<clang::CXXRecordDecl>(unit.resolver().resolve_tag(type));
            const auto* member = tag ? tag->getDefinition() : nullptr;
            bool copies = !member || copy_constructible(member, record);
            if(!copies && !move_constructible(member, record)) {
                return;
            }
            // Whether a dependent type copies, only its instantiation
            // tells: by value and moved, the parameter takes copyable and
            // move-only arguments alike.
            if(!copies || (type->isDependentType() && !type->isScalarType())) {
                moves = true;
            } else if(!type->isScalarType()) {
                type = context.getLValueReferenceType(type.withConst());
            }
        }
        auto name = field->getName();
        auto parameter = type_name(context, type, record, name);
        if(!parameter) {
            return;
        }
        if(index) {
            parameters += ", ";
            initializers += ", ";
        }
        parameters += *parameter;
        initializers +=
            moves ? std::format("{0}(std::move({0}))", name) : std::format("{0}({0})", name);
        moves_any |= moves;
    }
    auto line = std::format("{}{}({}) : {} {{}}",
                            fields.size() == 1 ? "explicit " : "",
                            record->getName(),
                            parameters,
                            initializers);

    auto members = insert_members(unit, record, {line});
    if(!members) {
        return;
    }
    std::vector<TextReplacement> edits;
    if(moves_any && !declares_std_move(context, record->getBeginLoc())) {
        auto insertion = include_insertion(unit);
        edits.push_back({
            .range = {insertion.offset, insertion.offset},
            .text = insertion.text("<utility>")
        });
    }
    edits.push_back(std::move(*members));
    out.push_back(CodeAction{
        .title = std::format("Generate a memberwise constructor for '{}'", record->getName()),
        .kind = protocol::CodeActionKind::RefactorRewrite,
        .edits = std::move(edits),
    });
}

}  // namespace clice::feature::action
