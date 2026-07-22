module;

#include <string>  // clang20+libstdc++ floor: befriended by an instantiated std template; cannot be re-exported (see deps/stdlib.cppm)

export module clice:index.tu_index;

import stdlib;
import llvm;
import clang;
import :compile.compilation_unit;
import :index.include_graph;
import :semantic.relation_kind;
import :semantic.symbol_kind;
import :support.bitmap;
import :syntax.token;

export namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

/// Visibility scope of a symbol, determining which level of the multi-level
/// symbol table stores it.
enum class SymbolScope : std::uint8_t {
    /// Can be referenced from any TU (external linkage).  Stored in ProjectIndex.
    External = 0,
    /// Can be referenced across files within one TU but not across TUs
    /// (internal linkage: static, anonymous namespace).  Stored in the main
    /// file's MergedIndex shard.
    TULocal = 1,
    /// Cannot be referenced from any other file (local variables, parameters,
    /// labels).  Stored in the defining file's MergedIndex shard.
    FileLocal = 2,
};

struct Relation {
    RelationKind kind;

    std::uint32_t padding = 0;

    LocalSourceRange range;

    SymbolHash target_symbol;

    constexpr void set_definition_range(LocalSourceRange range) {
        target_symbol = std::bit_cast<SymbolHash>(range);
    }

    constexpr auto definition_range() {
        return std::bit_cast<LocalSourceRange>(target_symbol);
    }
};

struct Occurrence {
    /// range of this occurrence.
    Range range;

    ///
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;
};

struct FileIndex {
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations;

    std::vector<Occurrence> occurrences;

    void lookup(std::uint32_t offset, llvm::function_ref<bool(const Occurrence&)> callback) const;

    void lookup(SymbolHash symbol,
                RelationKind kind,
                llvm::function_ref<bool(const Relation&)> callback) const;

    std::array<std::uint8_t, 32> hash();
};

struct Symbol {
    std::string name;

    SymbolKind kind;

    SymbolScope scope = SymbolScope::External;

    /// All files that referenced this symbol.
    Bitmap reference_files;

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

using SymbolTable = llvm::DenseMap<SymbolHash, Symbol>;

struct TUIndex {
    /// The building timestamp of this file.
    std::chrono::milliseconds built_at;

    /// The include information of this file.
    IncludeGraph graph;

    SymbolTable symbols;

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    /// File indices keyed by path_id, populated by from() for deserialized data.
    /// When built from AST, this is empty and file_indices (keyed by FileID) is used.
    llvm::DenseMap<std::uint32_t, FileIndex> path_file_indices;

    FileIndex main_file_index;

    /// Build the index for `unit`. With interested_only, only rows in
    /// the interested file are kept. Note that a full build over a unit
    /// compiled with a preamble PCH is not a production combination
    /// (background indexing compiles without PCH): rows landing in the
    /// PCH's loaded copy of the main file would serialize a second entry
    /// under the main path id.
    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    void serialize(llvm::raw_ostream& os) const;

    static TUIndex from(const void* data);
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

    /// Contextual doesn’t take part in hashing and equality.
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
