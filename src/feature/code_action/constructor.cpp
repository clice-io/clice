#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"

namespace clice::feature::action {

namespace {

/// Whether a derived class's constructor can leave `record` to its
/// default constructor: one it declares must be neither deleted nor
/// private; the implicit one exists only without user-declared
/// constructors, and is deleted by a reference or const member without
/// an initializer, or by a base or member that cannot default-construct
/// itself.
bool default_constructible(const clang::CXXRecordDecl* record) {
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
        auto type = field->getType();
        if(type->isReferenceType()) {
            return false;
        }
        const auto* member = type->getAsCXXRecordDecl();
        if(member ? !default_constructible(member) : type.isConstQualified()) {
            return false;
        }
    }
    return true;
}

}  // namespace

void memberwise_constructor(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record || !record->isThisDeclarationADefinition() || record->isUnion() ||
       record->isLambda() || record->getName().empty()) {
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
    std::string line;
    llvm::raw_string_ostream os(line);
    if(fields.size() == 1) {
        os << "explicit ";
    }
    os << record->getName() << '(';
    for(auto [index, field]: llvm::enumerate(fields)) {
        if(index) {
            os << ", ";
        }
        auto type = field->getType();
        std::optional<std::string> parameter;
        if(type->isReferenceType()) {
            parameter = type_name(context, type, record, field->getName());
        } else if(type.getUnqualifiedType()->isScalarType()) {
            parameter = type_name(context, type.getUnqualifiedType(), record, field->getName());
        } else {
            parameter =
                type_name(context,
                          context.getLValueReferenceType(type.getUnqualifiedType().withConst()),
                          record,
                          field->getName());
        }
        if(!parameter) {
            return;
        }
        os << *parameter;
    }
    os << ") : ";
    for(auto [index, field]: llvm::enumerate(fields)) {
        if(index) {
            os << ", ";
        }
        os << field->getName() << '(' << field->getName() << ')';
    }
    os << " {}";

    if(auto edit = insert_members(unit, record, {line})) {
        out.push_back(CodeAction{
            .title = std::format("Generate a memberwise constructor for '{}'", record->getName()),
            .kind = protocol::CodeActionKind::refactor_rewrite,
            .edits = {std::move(*edit)},
        });
    }
}

}  // namespace clice::feature::action
