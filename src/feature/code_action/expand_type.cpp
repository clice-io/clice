#include <array>
#include <format>
#include <string>
#include <vector>

#include "compile/compilation_unit.h"
#include "feature/code_action/action.h"
#include "semantic/types.h"

#include "llvm/ADT/STLExtras.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/TypeLoc.h"
#include "clang/Basic/TokenKinds.h"

namespace clice::feature::action {

namespace {

/// A declaration specifier spelled as a keyword, other than a type or a
/// cv-qualifier.
bool is_specifier(const clang::syntax::Token& token) {
    using enum clang::tok::TokenKind;
    constexpr std::array specifiers = {kw_static,
                                       kw_extern,
                                       kw_inline,
                                       kw_constexpr,
                                       kw_constinit,
                                       kw_consteval,
                                       kw_thread_local,
                                       kw_register,
                                       kw_friend};
    return llvm::is_contained(specifiers, token.kind());
}

}  // namespace

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
    // A cv-qualifier written before `auto` qualifies the deduced type; in
    // front of a pointer it would qualify the pointee, so it moves behind
    // the `*`. The edit reaches the qualifiers right before `auto`, with
    // nothing but spaces between them; one parted from it by other
    // specifiers, or possibly spelled by a macro among them, is out of
    // reach.
    auto content = unit.main_content();
    // Nullability attributes print after the `*` they qualify.
    const auto* declarator = deduced->getTypePtr();
    while(auto* attributed = llvm::dyn_cast<clang::AttributedType>(declarator)) {
        declarator = attributed->getModifiedType().getTypePtr();
    }
    if(llvm::isa<clang::PointerType, clang::MemberPointerType>(declarator)) {
        auto tokens = unit.spelled_tokens(unit.main_file());
        const auto* at = llvm::partition_point(tokens, [&](const clang::syntax::Token& token) {
            return unit.file_offset(token.location()) < range->begin;
        });
        const auto* first = at;
        while(first != tokens.begin() && is_cv(*std::prev(first))) {
            first -= 1;
        }
        for(const auto* it = first; it != tokens.begin(); it -= 1) {
            const auto& token = *std::prev(it);
            if(is_cv(token) || unit.token_buffer().expansionStartingAt(&token)) {
                return;
            }
            if(!is_specifier(token)) {
                break;
            }
        }
        for(const auto* it = first; it != at; it += 1) {
            auto end = unit.file_offset(it->endLocation());
            auto next = it + 1 == at ? range->begin : unit.file_offset((it + 1)->location());
            if(!content.substr(end, next - end).trim().empty()) {
                return;
            }
            printed += ' ';
            printed += clang::tok::getKeywordSpelling(it->kind());
        }
        range->begin = unit.file_offset(first->location());
    }
    out.push_back(CodeAction{
        .title = std::format("Replace '{}' with '{}'",
                             content.substr(range->begin, range->length()),
                             printed),
        .kind = protocol::CodeActionKind::refactor_rewrite,
        .edits = {{*range, std::move(printed)}},
    });
}

}  // namespace clice::feature::action
