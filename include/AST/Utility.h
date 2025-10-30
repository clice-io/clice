#pragma once

#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"

namespace clice::ast {

/// is this decl a definition?
bool is_definition(const clang::Decl* decl);

/// Check whether the decl is a template. Note that for partial specializations,
/// we consider it as a template while clang does not.
bool is_templated(const clang::Decl* decl);

/// Check whether the decl is anonymous.
bool is_anonymous(const clang::NamedDecl* decl);

/// Checks whether the location is inside the main file.
bool is_inside_main_file(clang::SourceLocation loc, const clang::SourceManager& sm);

/// Checks whether the decl is an implicit template instantiation.
bool is_implicit_template_instantiation(const clang::NamedDecl* decl);

/// Return the decl where it is instantiated from. If could be a template decl
/// or a member of a class template. If the decl is a full specialization, return
/// itself.
const clang::NamedDecl* instantiated_from(const clang::NamedDecl* decl);

const clang::NamedDecl* normalize(const clang::NamedDecl* decl);

llvm::StringRef identifier_of(const clang::NamedDecl& D);

llvm::StringRef identifier_of(clang::QualType T);

/// Get the name of the decl.
std::string name_of(const clang::NamedDecl* decl);

std::string display_name_of(const clang::NamedDecl* decl);

/// To response go-to-type-definition request. Some decls actually have a type
/// for example the result of `typeof(var)` is the type of `var`. This function
/// returns the type for the decl if any.
clang::QualType type_of(const clang::NamedDecl* decl);

/// Get the underlying decl for a type if any.
auto decl_of(clang::QualType type) -> const clang::NamedDecl*;

/// Recursively strips all pointers, references, and array extents from
/// a TypeLoc. e.g., for "const int*(&)[3]", the result will be location
/// "for int".
auto unwrap_type(clang::TypeLoc type, bool unwrap_function_type = true) -> clang::TypeLoc;

auto get_only_instantiation(clang::NamedDecl* templated_decl) -> clang::NamedDecl*;

auto get_only_instantiation(clang::ParmVarDecl* param) -> clang::ParmVarDecl*;

std::string summarize_expr(const clang::Expr* E);

// Returns the template parameter pack type that this parameter was expanded
// from (if in the Args... or Args&... or Args&&... form), if this is the case,
// nullptr otherwise.
const clang::TemplateTypeParmType* underlying_pack_type(const clang::ParmVarDecl* param);

// Returns the parameters that are forwarded from the template parameters.
// For example, `template <typename... Args> void foo(Args... args)` will return
// the `args` parameters.
auto resolve_forwarding_params(const clang::FunctionDecl* decl, unsigned max_depth = 10)
    -> llvm::SmallVector<const clang::ParmVarDecl*>;

// A simple wrapper for `clang::desugarForDiagnostic` that provides optional
// semantic.
std::optional<clang::QualType> desugar(clang::ASTContext& context, clang::QualType type);

// Apply a series of heuristic methods to determine whether or not a QualType QT
// is suitable for desugaring (e.g. getting the real name behind the using-alias
// name). If so, return the desugared type. Otherwise, return the unchanged
// parameter QT.
//
// This could be refined further. See
// https://github.com/clangd/clangd/issues/1298.
clang::QualType maybe_desugar(clang::ASTContext& context, clang::QualType type);

// Given a callee expression `Fn`, if the call is through a function pointer,
// try to find the declaration of the corresponding function pointer type,
// so that we can recover argument names from it.
// FIXME: This function is mostly duplicated in SemaCodeComplete.cpp; unify.
clang::FunctionProtoTypeLoc proto_type_loc(clang::Expr* expr);

}  // namespace clice::ast
