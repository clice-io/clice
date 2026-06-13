/// Ported from clangd's FindTarget.h (llvmorg-21.1.8), part of the LLVM
/// project, licensed under Apache License v2.0 with LLVM Exceptions.
/// See https://llvm.org/LICENSE.txt for license information.

#pragma once

#include <bitset>
#include <utility>

#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTTypeTraits.h"

namespace clang {

class HeuristicResolver;

}

namespace clice::ast {

/// Describes the link between an AST node and a Decl it refers to.
enum class DeclRelation : unsigned {
    /// Template options apply when the declaration is an instantiated template.
    /// e.g. [[vector<int>]] vec;

    /// This is the template instantiation that was referred to.
    /// e.g. template<> class vector<int> (the implicit specialization)
    TemplateInstantiation,
    /// This is the pattern the template specialization was instantiated from.
    /// e.g. class vector<T> (the pattern within the primary template)
    TemplatePattern,

    /// Alias options apply when the declaration is an alias.
    /// e.g. namespace client { [[X]] x; }

    /// This declaration is an alias that was referred to.
    /// e.g. using ns::X (the UsingDecl directly referenced),
    ///      using Z = ns::Y (the TypeAliasDecl directly referenced)
    Alias,
    /// This is the underlying declaration for a renaming-alias, decltype etc.
    /// e.g. class ns::Y (the underlying declaration referenced).
    ///
    /// Note that we don't treat `using ns::X` as a first-class declaration like
    /// `using Z = ns::Y`. Therefore reference to X that goes through this
    /// using-decl is considered a direct reference (without the Underlying bit).
    /// Nevertheless, we report `using ns::X` as an Alias, so that some features
    /// like go-to-definition can still target it.
    Underlying,
};

/// A bitfield of DeclRelations.
class DeclRelationSet {
    using Set = std::bitset<static_cast<unsigned>(DeclRelation::Underlying) + 1>;

    Set bits;

    DeclRelationSet(Set bits) : bits(bits) {}

public:
    DeclRelationSet() = default;

    DeclRelationSet(DeclRelation relation) {
        bits.set(static_cast<unsigned>(relation));
    }

    explicit operator bool() const {
        return bits.any();
    }

    friend DeclRelationSet operator&(DeclRelationSet lhs, DeclRelationSet rhs) {
        return DeclRelationSet(lhs.bits & rhs.bits);
    }

    friend DeclRelationSet operator|(DeclRelationSet lhs, DeclRelationSet rhs) {
        return DeclRelationSet(lhs.bits | rhs.bits);
    }

    friend bool operator==(DeclRelationSet lhs, DeclRelationSet rhs) {
        return lhs.bits == rhs.bits;
    }

    friend DeclRelationSet operator~(DeclRelationSet set) {
        return DeclRelationSet(~set.bits);
    }

    DeclRelationSet& operator|=(DeclRelationSet other) {
        bits |= other.bits;
        return *this;
    }

    DeclRelationSet& operator&=(DeclRelationSet other) {
        bits &= other.bits;
        return *this;
    }

    bool contains(DeclRelationSet other) const {
        return (bits & other.bits) == other.bits;
    }
};

/// The above operators can't be looked up if both sides are enums.
/// over.match.oper.html#3.2
inline DeclRelationSet operator|(DeclRelation lhs, DeclRelation rhs) {
    return DeclRelationSet(lhs) | DeclRelationSet(rhs);
}

inline DeclRelationSet operator&(DeclRelation lhs, DeclRelation rhs) {
    return DeclRelationSet(lhs) & DeclRelationSet(rhs);
}

inline DeclRelationSet operator~(DeclRelation relation) {
    return ~DeclRelationSet(relation);
}

/// target_decl() finds the declaration referred to by an AST node.
/// For example a RecordTypeLoc refers to the RecordDecl for the type.
///
/// In some cases there are multiple results, e.g. a dependent unresolved
/// OverloadExpr may have several candidates. All will be returned:
///
///    void foo(int);    <-- candidate
///    void foo(double); <-- candidate
///    template <typename T> callFoo() { foo(T()); }
///                                      ^ OverloadExpr
///
/// In other cases, there may be choices about what "referred to" means.
/// e.g. does naming a typedef refer to the underlying type?
/// The results are marked with a set of DeclRelations, and can be filtered.
///
///    struct S{};    <-- candidate (underlying)
///    using T = S{}; <-- candidate (alias)
///    T x;
///    ^ TypedefTypeLoc
///
/// Formally, we walk a graph starting at the provided node, and return the
/// decls that were found. Certain edges in the graph have labels, and for each
/// decl we return the set of labels seen on a path to the decl.
/// For the previous example:
///
///                TypedefTypeLoc T
///                       |
///                 TypedefType T
///                    /     \
///           [underlying]  [alias]
///                  /         \
///          RecordDecl S    TypeAliasDecl T
///
/// Note that this function only returns NamedDecls. Generally other decls
/// don't have references in this sense, just the node itself.
/// If callers want to support such decls, they should cast the node directly.
///
/// FIXME: some AST nodes cannot be DynTypedNodes, these cannot be specified.
auto target_decl(const clang::DynTypedNode& node,
                 DeclRelationSet mask,
                 const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1>;

/// Similar to target_decl(), however instead of applying a filter, all
/// possible decls are returned along with their DeclRelationSets.
/// This is suitable for indexing, where everything is recorded and filtering
/// is applied later.
auto all_target_decls(const clang::DynTypedNode& node, const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<std::pair<const clang::NamedDecl*, DeclRelationSet>, 1>;

/// Find declarations explicitly referenced in the source code defined by \p
/// node. For templates, will prefer to return a template instantiation
/// whenever possible. However, can also return a template pattern if the
/// specialization cannot be picked, e.g. in dependent code or when there is no
/// corresponding Decl for a template instantiation, e.g. for templated using
/// decls:
///    template <class T> using Ptr = T*;
///    Ptr<int> x;
///    ^~~ there is no Decl for 'Ptr<int>', so we return the template pattern.
/// \p mask should not contain TemplatePattern or TemplateInstantiation.
auto explicit_reference_targets(clang::DynTypedNode node,
                                DeclRelationSet mask,
                                const clang::HeuristicResolver* resolver)
    -> llvm::SmallVector<const clang::NamedDecl*, 1>;

}  // namespace clice::ast
