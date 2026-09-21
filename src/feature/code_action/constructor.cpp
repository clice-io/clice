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

void memberwise_constructor(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record || !record->isThisDeclarationADefinition() || record->isUnion() ||
       record->isLambda() || record->getName().empty()) {
        return;
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

    auto insertion = class_body_insertion(unit, record);
    if(!insertion) {
        return;
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
        if(type->isReferenceType()) {
            os << type_name(context, type, record);
        } else if(type.getUnqualifiedType()->isScalarType()) {
            os << type_name(context, type.getUnqualifiedType(), record);
        } else {
            os << "const " << type_name(context, type.getUnqualifiedType(), record) << '&';
        }
        os << ' ' << field->getName();
    }
    os << ") : ";
    for(auto [index, field]: llvm::enumerate(fields)) {
        if(index) {
            os << ", ";
        }
        os << field->getName() << '(' << field->getName() << ')';
    }
    os << " {}";

    auto content = unit.main_content();
    auto record_indent = line_indent(content, main_range(unit, record->getBeginLoc())->begin);
    std::string text;
    if(insertion->break_before) {
        text += '\n';
    }
    if(!ends_public(record)) {
        text += std::format("{}public:\n", record_indent);
    }
    text += std::format("{}{}\n", insertion->indent, line);
    if(insertion->break_before) {
        text += record_indent;
    }
    out.push_back(CodeAction{
        .id = "memberwise-constructor",
        .title = std::format("Generate a memberwise constructor for '{}'", record->getName()),
        .kind = CodeActionKind::RefactorRewrite,
        .edits = {{{insertion->offset, insertion->offset}, std::move(text)}},
    });
}

}  // namespace clice::feature::action
