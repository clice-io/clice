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

/// The declaration overriding `method` in `record`, its types spelled
/// fully qualified: the base's unqualified spellings need not resolve in
/// the derived class's scope. Nullopt when a type has no spelling.
std::optional<std::string> override_declaration(CompilationUnitRef unit,
                                                const clang::CXXMethodDecl* method,
                                                const clang::CXXRecordDecl* record) {
    auto& context = unit.context();
    std::string declarator;
    llvm::raw_string_ostream os(declarator);
    os << display::name_of(method, {.qualified = false}) << '(';
    for(auto [index, param]: llvm::enumerate(method->parameters())) {
        if(index) {
            os << ", ";
        }
        auto declaration = type_name(context, param->getOriginalType(), record, param->getName());
        if(!declaration) {
            return std::nullopt;
        }
        os << *declaration;
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
    if(llvm::isa<clang::CXXConversionDecl>(method)) {
        return declarator + " override;";
    }
    // A return type such as a function pointer wraps the declarator. The
    // type is printed around a placeholder rather than the declarator
    // itself: type_name rewrites its whole output (drops namespace
    // prefixes, binds `*` and `&` to the type), which must not reach the
    // parameters or the exception specification.
    constexpr llvm::StringRef placeholder = "clice_override_declarator";
    auto text = type_name(context, method->getReturnType(), record, placeholder);
    if(!text) {
        return std::nullopt;
    }
    text->replace(text->find(placeholder), placeholder.size(), declarator);
    return *text + " override;";
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
    std::vector<std::string> lines;
    llvm::SmallPtrSet<const clang::CXXMethodDecl*, 8> seen;
    for(const auto& [method, overriding]: overriders) {
        for(const auto& [subobject, finals]: overriding) {
            for(const auto& final: finals) {
                if(!final.Method->isPureVirtual() || final.Method->getParent() == record ||
                   !seen.insert(final.Method->getCanonicalDecl()).second) {
                    continue;
                }
                if(auto line = override_declaration(unit, final.Method, record)) {
                    lines.push_back(std::move(*line));
                }
            }
        }
    }
    if(lines.empty()) {
        return;
    }
    if(auto edit = insert_members(unit, record, lines)) {
        out.push_back(CodeAction{
            .title = std::format("Implement pure virtual methods of '{}'", record->getName()),
            .kind = protocol::CodeActionKind::refactor_rewrite,
            .edits = {std::move(*edit)},
        });
    }
}

}  // namespace clice::feature::action
