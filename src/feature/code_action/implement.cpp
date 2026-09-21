#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/display.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclCXX.h"

namespace clice::feature::action {

namespace {

/// The declaration overriding `method` in a derived class, its types
/// spelled fully qualified: the base's unqualified spellings need not
/// resolve in the derived class's scope.
std::string override_declaration(CompilationUnitRef unit,
                                 const clang::CXXMethodDecl* method,
                                 const clang::DeclContext* from) {
    auto& context = unit.context();
    std::string text;
    llvm::raw_string_ostream os(text);
    os << type_name(context, method->getReturnType(), from) << ' '
       << display::name_of(method, {.qualified = false}) << '(';
    for(auto [index, param]: llvm::enumerate(method->parameters())) {
        if(index) {
            os << ", ";
        }
        os << type_name(context, param->getOriginalType(), from);
        if(!param->getName().empty()) {
            os << ' ' << param->getName();
        }
    }
    if(method->isVariadic()) {
        os << ", ...";
    }
    os << ')';
    if(method->isConst()) {
        os << " const";
    }
    if(method->isVolatile()) {
        os << " volatile";
    }
    switch(method->getRefQualifier()) {
        case clang::RQ_None: break;
        case clang::RQ_LValue: os << (method->getMethodQualifiers().empty() ? " &" : "&"); break;
        case clang::RQ_RValue: os << (method->getMethodQualifiers().empty() ? " &&" : "&&"); break;
    }
    if(auto spec = spelled_text(unit, method->getExceptionSpecSourceRange())) {
        os << ' ' << *spec;
    }
    os << " override;";
    return text;
}

}  // namespace

void implement_pure_virtuals(const Context& ctx, std::vector<CodeAction>& out) {
    auto unit = ctx.unit;
    const auto* record = ctx.node.get<clang::CXXRecordDecl>();
    if(!record || !record->isThisDeclarationADefinition() || record->isDependentType() ||
       record->getNumBases() == 0 || !record->isAbstract()) {
        return;
    }

    clang::CXXFinalOverriderMap overriders;
    record->getFinalOverriders(overriders);
    std::vector<const clang::CXXMethodDecl*> pending;
    llvm::SmallPtrSet<const clang::CXXMethodDecl*, 8> seen;
    for(const auto& [method, overriding]: overriders) {
        for(const auto& [subobject, finals]: overriding) {
            for(const auto& final: finals) {
                if(final.Method->isPureVirtual() && final.Method->getParent() != record &&
                   seen.insert(final.Method->getCanonicalDecl()).second) {
                    pending.push_back(final.Method);
                }
            }
        }
    }
    if(pending.empty()) {
        return;
    }

    auto insertion = class_body_insertion(unit, record);
    if(!insertion) {
        return;
    }
    auto content = unit.main_content();
    auto record_indent = line_indent(content, main_range(unit, record->getBeginLoc())->begin);

    std::string text;
    if(insertion->break_before) {
        text += '\n';
    }
    if(!ends_public(record)) {
        text += std::format("{}public:\n", record_indent);
    }
    for(const auto* method: pending) {
        text +=
            std::format("{}{}\n", insertion->indent, override_declaration(unit, method, record));
    }
    if(insertion->break_before) {
        text += record_indent;
    }
    out.push_back(CodeAction{
        .id = "implement-pure-virtuals",
        .title = std::format("Implement pure virtual methods of '{}'", record->getName()),
        .kind = CodeActionKind::RefactorRewrite,
        .edits = {{{insertion->offset, insertion->offset}, std::move(text)}},
    });
}

}  // namespace clice::feature::action
