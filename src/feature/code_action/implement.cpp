#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/display.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclCXX.h"

namespace clice::feature::action {

namespace {

/// Whether an override of `method` must be noexcept; nullopt when that
/// depends on a specification not instantiated yet, or on a dynamic one.
std::optional<bool> requires_noexcept(const clang::CXXMethodDecl* method) {
    const auto* proto = method->getType()->castAs<clang::FunctionProtoType>();
    // A class template's member gets its specification instantiated on
    // first need; one written without template arguments is known already.
    if(proto->getExceptionSpecType() == clang::EST_Uninstantiated) {
        proto = proto->getExceptionSpecTemplate()->getType()->castAs<clang::FunctionProtoType>();
    }
    if(proto->getExceptionSpecType() == clang::EST_Dynamic) {
        return std::nullopt;
    }
    switch(proto->canThrow()) {
        case clang::CT_Cannot: return true;
        case clang::CT_Can: return false;
        case clang::CT_Dependent: return std::nullopt;
    }
    std::unreachable();
}

/// The declaration overriding `method` in `record`, its types spelled for
/// `record`: the base's spellings need not resolve in the derived class's
/// scope. Nullopt when a type has no spelling there.
std::optional<std::string> override_declaration(clang::ASTContext& context,
                                                const clang::CXXMethodDecl* method,
                                                const clang::CXXRecordDecl* record,
                                                bool is_nothrow) {
    std::string text;
    llvm::raw_string_ostream os(text);
    if(method->isConsteval()) {
        os << "consteval ";
    }
    if(!llvm::isa<clang::CXXConversionDecl>(method)) {
        auto result = type_name(context, method->getReturnType(), record);
        if(!result) {
            return std::nullopt;
        }
        os << *result << ' ';
    }
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
        os << (method->param_empty() ? "..." : ", ...");
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
    if(is_nothrow) {
        os << " noexcept";
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

    // One declaration overrides the functions of every base sharing its
    // name and signature, and is noexcept when any of them is.
    auto& context = unit.context();
    llvm::SmallVector<llvm::SmallVector<const clang::CXXMethodDecl*, 1>, 8> groups;
    clang::CXXFinalOverriderMap overriders;
    record->getFinalOverriders(overriders);
    for(const auto& [method, overriding]: overriders) {
        for(const auto& [subobject, finals]: overriding) {
            for(const auto& final: finals) {
                if(!final.Method->isPureVirtual() || final.Method->getParent() == record) {
                    continue;
                }
                auto* group = llvm::find_if(groups, [&](const auto& group) {
                    return group.front()->getDeclName() == final.Method->getDeclName() &&
                           context.hasSameFunctionTypeIgnoringExceptionSpec(
                               group.front()->getType(),
                               final.Method->getType());
                });
                if(group == groups.end()) {
                    groups.emplace_back().push_back(final.Method);
                } else if(!llvm::is_contained(*group, final.Method)) {
                    group->push_back(final.Method);
                }
            }
        }
    }

    std::vector<std::string> lines;
    for(const auto& group: groups) {
        auto specs = llvm::map_range(group, requires_noexcept);
        bool is_nothrow = llvm::is_contained(specs, std::optional(true));
        if(!is_nothrow && llvm::is_contained(specs, std::nullopt)) {
            continue;
        }
        if(auto line = override_declaration(context, group.front(), record, is_nothrow)) {
            lines.push_back(std::move(*line));
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
