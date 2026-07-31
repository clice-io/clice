#pragma once

#include <optional>

#include "llvm/ADT/SmallVector.h"
#include "clang/AST/Decl.h"
#include "clang/AST/TypeLoc.h"

/// Type-centric AST helpers: mapping types to the declarations they refer
/// to (dependent or not), type unwrapping and deduction queries.
/// Declaration navigation lives in decls.h, rendering in display.h.
namespace clice::types {

class TemplateResolver;

/// The declarations a (possibly dependent) type refers to.
///
/// Concrete types extract their underlying declaration structurally — at
/// most one. Dependent types are resolved heuristically through the
/// resolver's pseudo-instantiation and may yield several candidates;
/// without a resolver they yield none.
auto decls_of(clang::QualType type, TemplateResolver* resolver = nullptr)
    -> llvm::SmallVector<const clang::NamedDecl*, 1>;

/// The first declaration of decls_of, null if none: for consumers that
/// only present a single target.
auto decl_of(clang::QualType type, TemplateResolver* resolver = nullptr) -> const clang::NamedDecl*;

/// Recursively strips all pointers, references, and array extents from a
/// TypeLoc. e.g., for "const int*(&)[3]", the result will be the location
/// of "int".
auto unwrap(clang::TypeLoc type, bool unwrap_function_type = true) -> clang::TypeLoc;

/// Return the type a TypeDecl declares, preferring the sugared form with
/// template arguments as written for class template specializations.
auto declared_type(const clang::TypeDecl* decl) -> clang::QualType;

/// Compute the type deduced for the `auto` or `decltype` written at the
/// given location, if it has been deduced.
auto deduced_type(clang::ASTContext& context, clang::SourceLocation loc)
    -> std::optional<clang::QualType>;

}  // namespace clice::types
