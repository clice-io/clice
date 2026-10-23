module;

#include "support/prelude.h"

module clice;

import :compile.compilation;
import :feature.code_action.action;
import :semantic.types;
import :syntax.lexical_scan;

namespace clice::feature::action {

namespace {

/// A declaration specifier spelled as a keyword, other than a type or a
/// cv-qualifier.
bool is_specifier(TokenKind kind) {
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
    return ranges::contains(specifiers, kind);
}

}  // namespace

void expand_deduced_type(const Context& ctx, std::vector<CodeAction>& out) {
    auto& unit = ctx.unit;
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
    // `auto x;` leaves its placeholder undeduced, a type that desugars to
    // itself.
    if(deduced.isNull() || deduced->isDependentType() || deduced->isUndeducedType()) {
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
    auto range = editable_range(unit, written);
    if(!range) {
        return;
    }
    // Only a type spelled before its declarator can stand in for `auto`:
    // `int (*)(int)` has nowhere to put the name.
    auto declaration =
        type_name(unit.context(), deduced, &ctx.node.decl_context(), written.getBegin(), "x");
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
    auto content = unit.focus_file().content();
    // The deduced type prints through the sugar of another deduction and
    // of decltype, and a nullability attribute after the `*` it qualifies.
    auto declarator = deduced;
    while(llvm::isa<clang::AutoType, clang::DecltypeType, clang::AttributedType>(declarator)) {
        declarator = declarator->getLocallyUnqualifiedSingleStepDesugaredType();
    }
    if(llvm::isa<clang::PointerType, clang::MemberPointerType>(declarator)) {
        auto focus = unit.focus_file();
        auto& token_map = focus.tokens();
        auto tokens = token_map.spelled();
        std::size_t at = token_map.spelled_at_or_after(range->begin);
        auto first = at;
        for(auto it = code_before(tokens, at); it && is_cv(focus.kind(tokens[*it]));
            it = code_before(tokens, *it)) {
            first = *it;
        }
        auto starts_expansion = [&](const Token& token) {
            auto expansions = token_map.expansions_overlapping(llvm::ArrayRef(token));
            return !expansions.empty() && expansions.front().spelled.begin() == &token;
        };
        for(auto it = code_before(tokens, first); it; it = code_before(tokens, *it)) {
            const auto& token = tokens[*it];
            if(is_cv(focus.kind(token)) || starts_expansion(token)) {
                return;
            }
            if(!is_specifier(focus.kind(token))) {
                break;
            }
        }
        for(auto index = first; index < at; index += 1) {
            const auto& token = tokens[index];
            auto end = token.range.end;
            auto next = index + 1 == at ? range->begin : tokens[index + 1].range.begin;
            if(token.kind == clang::tok::comment ||
               !content.substr(end, next - end).trim().empty()) {
                return;
            }
            // decltype of a const pointer has the qualifier already.
            auto kind = focus.kind(token);
            bool has = kind == clang::tok::kw_const ? deduced.isConstQualified()
                                                    : deduced.isVolatileQualified();
            if(!has) {
                printed += ' ';
                printed += clang::tok::getKeywordSpelling(kind);
            }
        }
        range->begin = tokens[first].range.begin;
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
