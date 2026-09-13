#pragma once

/// Declaration identity: the entity hash that keys the symbol index, equal
/// across translation units and processes for declarations the compiler
/// would merge as one entity (ASTContext::isSameEntity), and different for
/// declarations it keeps apart (overloads, template heads, constraints,
/// specialization arguments). The rules live in the identity objective
/// document; this header only names the pieces.

#include <cstdint>

#include "compile/compilation_unit.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "clang/AST/Decl.h"

namespace clice {

/// One per compilation: every hash is memoized for the life of the AST, so
/// each declaration, canonical type and expression is encoded once.
class EntityTable {
public:
    explicit EntityTable(CompilationUnitRef unit) : unit(unit) {}

    EntityTable(const EntityTable&) = delete;
    EntityTable& operator=(const EntityTable&) = delete;

    /// The entity of a declaration as given. Occurrences normalize first
    /// (decls::normalize) so every redeclaration and instantiation of a
    /// symbol shares one hash; contexts, types and expressions pass the
    /// declaration they actually refer to, which keeps a specialization
    /// apart from its template.
    std::uint64_t entity(const clang::NamedDecl* decl);

    /// The entity of a macro: its name and where the `#define` sits, as the
    /// canonical path of the file and the offset in it. A builtin or
    /// command-line macro has no file and is its name alone.
    std::uint64_t entity(llvm::StringRef name, clang::SourceLocation definition);

    /// The entity of a named module, from its full name.
    std::uint64_t module_entity(llvm::StringRef name);

    /// The entity of the symbol a declaration is named in — its enclosing
    /// named namespace, class, enum or function, normalized like an
    /// occurrence's symbol — or 0 at the translation unit and for
    /// C-linkage declarations, which every namespace shares. This is the
    /// chain a qualified name is spelled with; unlike the context inside
    /// an entity it skips anonymous namespaces and knows no modules.
    std::uint64_t parent(const clang::NamedDecl* decl);

    /// The hash of a type, taken canonically.
    std::uint64_t type_hash(clang::QualType type);

    /// The hash of an expression: clang's canonical profile with stable
    /// leaves (ExprHasher).
    std::uint64_t expr_hash(const clang::Expr* expr);

private:
    class Hasher;
    class Leaves;

    void add_context(Hasher& hasher, const clang::Decl* decl);
    void add_self(Hasher& hasher, const clang::NamedDecl* decl);
    void add_function(Hasher& hasher, const clang::FunctionDecl* function);
    void add_location(Hasher& hasher, clang::SourceLocation location);
    void add_macro_history(Hasher& hasher, clang::SourceLocation location);
    void add_path(Hasher& hasher, clang::SourceLocation location);
    void add_declaration_name(Hasher& hasher, clang::DeclarationName name);
    void add_type(Hasher& hasher, clang::QualType type);
    void add_expr(Hasher& hasher, const clang::Expr* expr);
    void add_nested_name_specifier(Hasher& hasher, clang::NestedNameSpecifier specifier);
    void add_template_name(Hasher& hasher, clang::TemplateName name);
    void add_template_argument(Hasher& hasher, const clang::TemplateArgument& argument);
    void add_template_arguments(Hasher& hasher, llvm::ArrayRef<clang::TemplateArgument> arguments);
    void add_template_head(Hasher& hasher, const clang::TemplateParameterList* parameters);
    void add_value(Hasher& hasher, const clang::APValue& value);

    CompilationUnitRef unit;

    llvm::DenseMap<const clang::Decl*, std::uint64_t> entities;
    llvm::DenseSet<const clang::Decl*> in_progress;
    llvm::DenseMap<const void*, std::uint64_t> types;
    llvm::DenseMap<const clang::Expr*, std::uint64_t> exprs;
};

}  // namespace clice
