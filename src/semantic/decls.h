#pragma once

#include "llvm/ADT/SmallVector.h"
#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"

/// Declaration-centric AST helpers: template predicates, instantiation
/// navigation and call/parameter analysis. Type queries live in types.h,
/// rendering in display.h.
namespace clice::decls {

/// Check whether the decl is a template. Note that for partial
/// specializations, we consider it as a template while clang does not.
bool is_templated(const clang::Decl* decl);

/// Check whether the decl is an implicit template instantiation.
bool is_implicit_instantiation(const clang::NamedDecl* decl);

/// Check whether the decl heads a template-instantiation subtree: an
/// implicit instantiation, or the decl an explicit instantiation directive
/// creates. Everything instantiated reuses the pattern's source locations;
/// of the whole subtree only the explicit directive itself is written.
///
/// FIXME(explicit-instantiation): clang mislocates the function and
/// variable directive forms at the pattern. The class form only works as
/// long as each written directive happens to own its own specialization
/// redecl — the written info lives on the specialization, not the
/// directive, so any node reuse loses a directive's locations. clang 23's
/// ExplicitInstantiationDecl (llvm/llvm-project#191658) records each
/// written directive, letting features stop special-casing all forms.
bool is_instantiation(const clang::Decl* decl);

/// Check whether the decl is a member of a class template instantiation,
/// instantiated from the pattern's corresponding member. Such members
/// repeat the directive's specialization kind when the instantiation is
/// explicit, but unlike the directive's own decl nothing about them is
/// written.
bool is_member_specialization(const clang::Decl* decl);

/// Return the decl where it is instantiated from. It could be a template
/// decl or a member of a class template. If the decl is a full
/// specialization, return itself.
auto instantiated_from(const clang::NamedDecl* decl) -> const clang::NamedDecl*;

/// The canonical decl, folding instantiations back to the canonical decl
/// of their pattern.
auto normalize(const clang::NamedDecl* decl) -> const clang::NamedDecl*;

/// If exactly one non-explicit specialization of the templated decl
/// exists, return it, nullptr otherwise.
auto only_instantiation(clang::NamedDecl* templated_decl) -> clang::NamedDecl*;

/// Same for a parameter of a function template: the corresponding
/// parameter in the template's only instantiation.
auto only_instantiation(clang::ParmVarDecl* param) -> clang::ParmVarDecl*;

/// Returns the template parameter pack type that this parameter was
/// expanded from (if in the Args... or Args&... or Args&&... form), if
/// this is the case, nullptr otherwise.
auto underlying_pack_type(const clang::ParmVarDecl* param) -> const clang::TemplateTypeParmType*;

/// Returns the parameters that are forwarded from the template
/// parameters. For example, `template <typename... Args> void foo(Args...
/// args)` will return the `args` parameters.
auto resolve_forwarding_params(const clang::FunctionDecl* decl, unsigned max_depth = 10)
    -> llvm::SmallVector<const clang::ParmVarDecl*>;

/// Given a callee expression, if the call is through a function pointer,
/// try to find the declaration of the corresponding function pointer
/// type, so that we can recover argument names from it.
/// FIXME: This function is mostly duplicated in SemaCodeComplete.cpp; unify.
auto proto_type_loc(clang::Expr* expr) -> clang::FunctionProtoTypeLoc;

}  // namespace clice::decls
