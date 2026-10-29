module clice:semantic.types;

import :semantic.resolver;
import :support.prelude;

/// Type-centric AST helpers: mapping types to the declarations they refer
/// to (dependent or not), type unwrapping and deduction queries.
/// Declaration navigation lives in decls.h, rendering in display.h.
namespace clice::types {

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

/// The element type behind every pointer, reference and array layer
/// written in a type, e.g. `const int` for `const int*(&)[3]`, seeing
/// through what a deduced `auto` or a `decltype` stands for. A layer behind
/// a type alias stays: the alias is what the type names.
auto unwrap(clang::QualType type) -> clang::QualType;

/// The destructor a variable or temporary of `type` runs, if a non-trivial
/// one exists.
auto destructor_of(clang::QualType type) -> const clang::CXXDestructorDecl*;

/// Return the type a TypeDecl declares, preferring the sugared form with
/// template arguments as written for class template specializations.
auto declared_type(const clang::TypeDecl* decl) -> clang::QualType;

/// The type a placeholder stands for. `written` is the node owning its
/// keyword and `owner` the declaration at or above it, never null:
/// - an `auto` or `decltype(auto)` of a declarator, the type deduced for it;
///   of a return type (`operator auto` included), the deduced return type;
/// - a `decltype(expr)`, the type it names;
/// - the `auto` of an abbreviated template's parameter, the type the
///   template's only instantiation took;
/// - a function whose leading `auto` announces a trailing return type,
///   that type.
/// An `auto` no deduction filled in stays the undeduced `auto` type; null
/// where `written` holds none of these or no single instantiation exists.
auto deduced_type(clang::DynTypedNode written, const clang::Decl* owner) -> clang::QualType;

/// The dependent class template specialization `access` names a member of,
/// resolved: `vector<Point<T>>` for `pts.push_back`; null when it names none.
auto accessed_specialization(const clang::CXXDependentScopeMemberExpr& access,
                             TemplateResolver& resolver)
    -> const clang::TemplateSpecializationType*;

/// Whether code could write `type` where it stands: nothing in it waits for
/// instantiation to say what an expression's type is (`<dependent type>`,
/// a dependent `decltype`, an undeduced `auto`), and no template parameter
/// in it is one an abbreviated template invented for an `auto`.
auto writable(clang::QualType type) -> bool;

/// Whether `argument` can never copy-initialize a parameter of type
/// `parameter`: no standard conversion, converting constructor or
/// conversion function reaches it. False wherever that is not certain: a
/// dependent type on either side, a braced list, a constructor or
/// conversion function template, a class whose definition the answer
/// needs and does not have.
auto never_initializes(clang::ASTContext& context,
                       const clang::Expr& argument,
                       clang::QualType parameter) -> bool;

}  // namespace clice::types
