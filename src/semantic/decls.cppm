module;

#include "modules/prelude.h"

module clice:semantic.decls;

/// Declaration-centric AST helpers: template predicates, instantiation
/// navigation and call/parameter analysis. Type queries live in types.h,
/// rendering in display.h.
namespace clice::decls {

/// Check whether the decl is a template. Note that for partial
/// specializations, we consider it as a template while clang does not.
bool is_templated(const clang::Decl* decl);

/// Check whether the decl names an entity its module exports: declared at
/// namespace scope by an `export` declaration, or a namespace holding one.
/// Members of an exported class are not exported entities of their own.
bool is_exported(const clang::Decl* decl);

/// Check whether the decl is an implicit template instantiation.
bool is_implicit_instantiation(const clang::NamedDecl* decl);

/// Check whether the decl heads a template-instantiation subtree: an
/// implicit instantiation, or the specialization an explicit instantiation
/// directive creates. Everything instantiated reuses the pattern's source
/// locations; the directive itself is a written decl of its own
/// (ExplicitInstantiationDecl).
bool is_instantiation(const clang::Decl* decl);

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

/// Whether an argument for `param` is used through a mutable lvalue
/// reference, given `forwarded`, the parameter resolve_forwarding_params
/// resolves it to (`param` itself outside a pack). The argument's
/// eventual use is what counts, approximated as follows:
/// 1. the value category can only change from rvalue to lvalue during
///    forwarding, so `param` and `forwarded` both being lvalue references
///    means the category was preserved;
/// 2. an argument copied or cast somewhere along the chain can only reach
///    an rvalue or const lvalue reference parameter, so a mutable lvalue
///    reference at the end means it never was.
/// A pack parameter left unresolved says nothing about the eventual use.
bool binds_mutable_reference(const clang::ParmVarDecl* param, const clang::ParmVarDecl* forwarded);

/// Given a callee expression, if the call is through a function pointer,
/// try to find the declaration of the corresponding function pointer
/// type, so that we can recover argument names from it.
/// FIXME: This function is mostly duplicated in SemaCodeComplete.cpp; unify.
auto proto_type_loc(clang::Expr* expr) -> clang::FunctionProtoTypeLoc;

}  // namespace clice::decls
