#pragma once

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/NestedNameSpecifier.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"

namespace clang {

class Decl;
class NamedDecl;

}  // namespace clang

namespace clice {

class TemplateResolver;

/// Information about a reference written in the source code, independent of
/// the AST node that contains it.
struct ReferenceLoc {
    /// Qualifier written in the source code, e.g. `ns::` for `ns::foo`.
    clang::NestedNameSpecifierLoc Qualifier;

    /// Start location of the last name part, e.g. `foo` in `ns::foo<int>`.
    clang::SourceLocation NameLoc;

    /// True when the reference is introducing a declaration or definition.
    bool IsDecl = false;

    /// The declarations referenced by the written name.
    llvm::SmallVector<const clang::NamedDecl*, 1> Targets;
};

enum class DeclRelation : unsigned {
    /// The written name is an alias that should be preserved in results.
    Alias = 1u << 0,
    /// The target was reached by desugaring or following the aliased entity.
    Underlying = 1u << 1,
    /// The target is a concrete template instantiation.
    TemplateInstantiation = 1u << 2,
    /// The target is the template pattern underlying an instantiation.
    TemplatePattern = 1u << 3,
};

struct DeclRelationSet {
    unsigned bits = 0;

    constexpr DeclRelationSet() = default;

    constexpr DeclRelationSet(DeclRelation relation) : bits(static_cast<unsigned>(relation)) {}

    constexpr explicit DeclRelationSet(unsigned bits) : bits(bits) {}

    constexpr bool contains(DeclRelationSet other) const {
        return (bits & other.bits) == other.bits;
    }

    constexpr bool contains(DeclRelation relation) const {
        return (bits & static_cast<unsigned>(relation)) != 0;
    }

    constexpr explicit operator bool() const {
        return bits != 0;
    }

    constexpr DeclRelationSet& operator|=(DeclRelationSet other) {
        bits |= other.bits;
        return *this;
    }

    constexpr DeclRelationSet& operator|=(DeclRelation relation) {
        bits |= static_cast<unsigned>(relation);
        return *this;
    }
};

constexpr DeclRelationSet operator|(DeclRelationSet lhs, DeclRelationSet rhs) {
    return DeclRelationSet(lhs.bits | rhs.bits);
}

constexpr DeclRelationSet operator|(DeclRelationSet lhs, DeclRelation rhs) {
    return lhs | DeclRelationSet(rhs);
}

constexpr DeclRelationSet operator|(DeclRelation lhs, DeclRelationSet rhs) {
    return DeclRelationSet(lhs) | rhs;
}

constexpr DeclRelationSet operator|(DeclRelation lhs, DeclRelation rhs) {
    return DeclRelationSet(lhs) | rhs;
}

constexpr DeclRelationSet operator&(DeclRelationSet lhs, DeclRelationSet rhs) {
    return DeclRelationSet(lhs.bits & rhs.bits);
}

constexpr DeclRelationSet operator&(DeclRelationSet lhs, DeclRelation rhs) {
    return lhs & DeclRelationSet(rhs);
}

struct TargetDecl {
    const clang::NamedDecl* Decl = nullptr;
    DeclRelationSet Relations;
};

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, ReferenceLoc ref);

/// Finds all declarations a selected AST node may refer to, including alias
/// and template-instantiation relationships that higher-level APIs may filter.
auto all_target_decls(const clang::DynTypedNode& node, TemplateResolver* resolver = nullptr)
    -> llvm::SmallVector<TargetDecl, 1>;

/// Recursively traverses \p stmt and reports all references explicitly written in
/// the source code.
void explicit_references(const clang::Stmt* stmt,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver = nullptr);

/// Recursively traverses \p decl and reports all references explicitly written in
/// the source code.
void explicit_references(const clang::Decl* decl,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver = nullptr);

/// Recursively traverses the full AST and reports all references explicitly
/// written in the source code.
void explicit_references(const clang::ASTContext& ast,
                         llvm::function_ref<void(ReferenceLoc)> out,
                         TemplateResolver* resolver = nullptr);

}  // namespace clice
