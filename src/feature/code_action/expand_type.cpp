module;

#include "modules/prelude.h"

module clice;

import :compile.compilation_unit;
import :feature.code_action.action;
import :semantic.types;

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
    const auto* owner = ctx.node.owning_decl();
    if(llvm::isa<clang::DecompositionDecl>(owner)) {
        return;
    }

    auto inner = types::unwrap(*loc);
    clang::SourceRange written;
    if(auto auto_loc = inner.getAs<clang::AutoTypeLoc>()) {
        if(auto_loc.isDecltypeAuto()) {
            return;
        }
        written = auto_loc.getLocalSourceRange();
    } else if(auto decltype_loc = inner.getAs<clang::DecltypeTypeLoc>()) {
        written = decltype_loc.getLocalSourceRange();
    } else {
        return;
    }
    auto deduced = types::deduced_type(clang::DynTypedNode::create(inner), owner);
    if(deduced.isNull() || deduced->isDependentType()) {
        return;
    }
    // `auto&&` bound to an lvalue deduces a reference: the written `&&`
    // goes with the `auto`, or the result would be a reference to a
    // reference.
    if(deduced->isReferenceType()) {
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
    auto declaration = type_name(unit.context(), deduced, &ctx.node.decl_context(), "x");
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
    // The deduced type prints through the sugar of another deduction and
    // of decltype, and a nullability attribute after the `*` it qualifies.
    auto declarator = deduced;
    while(llvm::isa<clang::AutoType, clang::DecltypeType, clang::AttributedType>(declarator)) {
        declarator = declarator->getLocallyUnqualifiedSingleStepDesugaredType();
    }
    if(llvm::isa<clang::PointerType, clang::MemberPointerType>(declarator)) {
        auto tokens = unit.spelled_tokens();
        const auto* at = llvm::partition_point(tokens, [&](const clang::syntax::Token& token) {
            return unit.file_offset(token.location()) < range->begin;
        });
        const auto* first = at;
        while(first != tokens.begin() && is_cv(*std::prev(first))) {
            first -= 1;
        }
        auto starts_expansion = [&](const clang::syntax::Token& token) {
            auto expansions = unit.expansions_overlapping(llvm::ArrayRef(token));
            return !expansions.empty() && expansions.front().spelled.begin() == &token;
        };
        for(const auto* it = first; it != tokens.begin(); it -= 1) {
            const auto& token = *std::prev(it);
            if(is_cv(token) || starts_expansion(token)) {
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
            // decltype of a const pointer has the qualifier already.
            bool has = it->kind() == clang::tok::kw_const ? deduced.isConstQualified()
                                                          : deduced.isVolatileQualified();
            if(!has) {
                printed += ' ';
                printed += clang::tok::getKeywordSpelling(it->kind());
            }
        }
        range->begin = unit.file_offset(first->location());
    }
    out.push_back(CodeAction{
        .title = std::format("Replace '{}' with '{}'",
                             content.substr(range->begin, range->length()),
                             printed),
        .kind = action_kind::expand_deduced_type,
        .edits = {{*range, std::move(printed)}},
    });
}

}  // namespace clice::feature::action
