#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"

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

/// Whether another class's constructor may call `ctor`.
bool callable(const clang::CXXConstructorDecl* ctor) {
    return !ctor->isDeleted() && ctor->getAccess() == clang::AS_public;
}

/// Whether a `const T&` argument copy-constructs the class. One not yet
/// declared is the implicit copy constructor, deleted per the class's
/// flags or by a user-declared move operation.
bool copy_constructible(const clang::CXXRecordDecl* record) {
    for(const auto* ctor: record->ctors()) {
        unsigned qualifiers = 0;
        if(ctor->isCopyConstructor(qualifiers) && (qualifiers & clang::Qualifiers::Const)) {
            return callable(ctor);
        }
    }
    return record->hasSimpleCopyConstructor() && !record->hasUserDeclaredMoveOperation();
}

/// Whether the class has a move constructor another class may call.
bool move_constructible(const clang::CXXRecordDecl* record) {
    for(const auto* ctor: record->ctors()) {
        if(ctor->isMoveConstructor()) {
            return callable(ctor);
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
            if(const auto* member = tag ? tag->getDefinition() : nullptr;
               member && !copy_constructible(member)) {
                if(!move_constructible(member)) {
                    return;
                }
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
        auto offset = include_insertion_offset(unit);
        edits.push_back({
            .range = {offset, offset},
            .text = "#include <utility>\n"
        });
    }
    edits.push_back(std::move(*members));
    out.push_back(CodeAction{
        .title = std::format("Generate a memberwise constructor for '{}'", record->getName()),
        .kind = protocol::CodeActionKind::refactor_rewrite,
        .edits = std::move(edits),
    });
}

}  // namespace clice::feature::action
