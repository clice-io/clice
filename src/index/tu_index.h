#pragma once

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "index/include_graph.h"
#include "semantic/relation_kind.h"
#include "semantic/symbol_kind.h"
#include "support/bitmap.h"

#include "kota/meta/annotation.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

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

    friend bool operator==(const Relation&, const Relation&) = default;

    friend auto operator<=>(const Relation&, const Relation&) = default;
};

struct Occurrence {
    /// range of this occurrence.
    Range range;

    ///
    SymbolHash target;

    friend bool operator==(const Occurrence&, const Occurrence&) = default;

    friend auto operator<=>(const Occurrence&, const Occurrence&) = default;
};

struct FileIndex {
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations;

    std::vector<Occurrence> occurrences;

    std::array<std::uint8_t, 32> hash();
};

struct Symbol {
    std::string name;

    SymbolKind kind;

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

    /// Runtime-only: keyed by AST-scoped `clang::FileID` during build; flushed
    /// into `path_file_indices` (keyed by path id) before serialization.
    kota::meta::skip<llvm::DenseMap<clang::FileID, FileIndex>> file_indices;

    /// File indices keyed by path_id. Populated from `file_indices` at
    /// serialize time, and directly from the wire on deserialize.
    llvm::DenseMap<std::uint32_t, FileIndex> path_file_indices;

    FileIndex main_file_index;

    static TUIndex build(CompilationUnitRef unit, bool interested_only = false);

    void serialize(llvm::raw_ostream& os);

    static TUIndex from(const void* data, std::size_t size);
};

}  // namespace clice::index
