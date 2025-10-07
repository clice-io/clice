#pragma once

#include "IncludeGraph.h"
#include "AST/SourceCode.h"
#include "AST/SymbolKind.h"
#include "AST/RelationKind.h"
#include "Support/Bitmap.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

struct Relation {
    RelationKind kind;

    std::uint32_t padding = 0;

    LocalSourceRange range;

    SymbolHash target_symbol;

    void set_definition_range(LocalSourceRange range) {
        target_symbol = std::bit_cast<SymbolHash>(range);
    }

    auto definition_range() {
        return std::bit_cast<LocalSourceRange>(target_symbol);
    }
};

struct Occurrence {
    /// range of this occurrence.
    Range range;

    ///
    SymbolHash target;
};

struct FileIndex {
    llvm::DenseMap<SymbolHash, std::vector<Relation>> relations;

    std::vector<Occurrence> occurrences;
};

struct Symbol {
    std::string name;

    SymbolKind kind;

    /// All files that referenced this symbol.
    Bitmap reference_files;
};

struct TUIndex {
    IncludeGraph graph;

    llvm::DenseMap<SymbolHash, Symbol> symbols;

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    static TUIndex build(CompilationUnit& unit);
};

}  // namespace clice::index
