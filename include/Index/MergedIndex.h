#pragma once

#include "TUIndex.h"

#define ROARING_EXCEPTIONS 0
#define ROARING_TERMINATE(message) std::abort()
#include "roaring/roaring.hh"

#include "llvm/Support/Allocator.h"

namespace clice::index {

/// struct CompilationContext {
///     /// The target of this compilation.
///     llvm::StringRef target;
///
///     /// The canonical compilation command.
///     llvm::StringRef command;
///
///     /// A version field for verification.
///     std::uint32_t version;
/// };
///
/// struct HeaderContext : CompilationContext {
///     /// The include location in the include graph.
///     std::uint32_t include;
///
///     /// The path of the file includes this header.
///     llvm::StringRef path;
/// };

struct HeaderContexts {
    std::uint32_t version = 0;

    using Context = std::pair<std::uint32_t, std::uint32_t>;

    /// A array of include location and its context id.
    llvm::SmallVector<Context> includes;
};

struct MergedIndex {
    /// For each merged index, we will give it a canonical id.
    /// The max canonical id.
    std::uint32_t max_canonical_id = 0;

    /// We use the value of SHA256 to judge whether two indices are same.
    /// Index with same content will be given same canonical id.
    llvm::DenseMap<llvm::StringRef, std::uint32_t> canonical_cache;

    /// The allocator for storing sha256 hash.
    llvm::BumpPtrAllocator allocator;

    /// The reference count of each canonical id.
    std::vector<std::uint32_t> canonical_ref_counts;

    /// The canonical id set of removed index.
    roaring::Roaring removed;

    /// A map between source file path and its header contexts.
    llvm::StringMap<HeaderContexts> contexts;

    /// All merged symbol relations.
    llvm::DenseMap<SymbolHash, llvm::DenseMap<Relation, roaring::Roaring>> relations;

    /// All merged symbol occurrences.
    llvm::DenseMap<Occurrence, roaring::Roaring> occurrences;

    void remove(llvm::StringRef path);

    void merge(llvm::StringRef path, std::uint32_t include, FileIndex& index);
};

}  // namespace clice::index

namespace llvm {

template <typename... Ts>
unsigned dense_hash(const Ts&... ts) {
    return llvm::DenseMapInfo<std::tuple<Ts...>>::getHashValue(std::tuple{ts...});
}

template <>
struct DenseMapInfo<clice::index::Occurrence> {
    using R = clice::LocalSourceRange;
    using V = clice::index::Occurrence;

    inline static V getEmptyKey() {
        return V(R(-1, 0), 0);
    }

    inline static V getTombstoneKey() {
        return V(R(-2, 0), 0);
    }

    static auto getHashValue(const V& v) {
        return dense_hash(v.range.begin, v.range.end, v.target);
    }

    static bool isEqual(const V& lhs, const V& rhs) {
        return lhs.range == rhs.range && lhs.target == rhs.target;
    }
};

template <>
struct DenseMapInfo<clice::index::Relation> {
    using R = clice::index::Relation;

    inline static R getEmptyKey() {
        return R{
            .kind = clice::RelationKind(),
            .range = clice::LocalSourceRange(-1, 0),
            .target_symbol = 0,
        };
    }

    inline static R getTombstoneKey() {
        return R{
            .kind = clice::RelationKind(),
            .range = clice::LocalSourceRange(-2, 0),
            .target_symbol = 0,
        };
    }

    /// Contextual doen't take part in hashing and equality.
    static auto getHashValue(const R& relation) {
        return dense_hash(relation.kind.value(),
                          relation.range.begin,
                          relation.range.end,
                          relation.target_symbol);
    }

    static bool isEqual(const R& lhs, const R& rhs) {
        return lhs.kind == rhs.kind && lhs.range == rhs.range &&
               lhs.target_symbol == rhs.target_symbol;
    }
};

}  // namespace llvm
