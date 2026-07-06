#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "index/include_graph.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

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

    /// Ordering defines the serialized wire order of relation maps.
    friend constexpr auto operator<=>(const Relation&, const Relation&) = default;
};

struct Occurrence {
    /// range of this occurrence.
    Range range;

    ///
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;

    /// (begin, end, target) ordering — the serialized wire order of occurrence
    /// maps, which the lazy buffer lookup binary-searches.
    friend constexpr auto operator<=>(const Occurrence&, const Occurrence&) = default;
};

/// Visit every occurrence whose range contains `offset` in a sequence sorted
/// by Occurrence's (begin, end, target) ordering; stops early when the
/// callback returns false.
void lookup_occurrences(std::span<const Occurrence> occurrences,
                        std::uint32_t offset,
                        llvm::function_ref<bool(const Occurrence&)> callback);

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

    /// Per-file indexes keyed by path id (files of the same path merge into
    /// one entry); the main file lives in main_file_index instead.
    llvm::DenseMap<std::uint32_t, FileIndex> path_file_indices;

    FileIndex main_file_index;

    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    void serialize(llvm::raw_ostream& os) const;

    static TUIndex from(const void* data, std::size_t size);
};

}  // namespace clice::index
