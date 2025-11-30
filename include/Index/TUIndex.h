#pragma once

#include <chrono>

#include "IncludeGraph.h"
#include "AST/RelationKind.h"
#include "AST/SourceCode.h"
#include "AST/SymbolKind.h"
#include "Support/Bitmap.h"

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

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    FileIndex main_file_index;

    static TUIndex build(CompilationUnit& unit);
};

}  // namespace clice::index
