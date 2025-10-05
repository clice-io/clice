#pragma once

#include "IncludeGraph.h"
#include "AST/SourceCode.h"
#include "AST/RelationKind.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolHash = std::uint64_t;

struct Relation {
    RelationKind kind;

    char padding[4] = {0, 0, 0, 0};

    LocalSourceRange range;

    union {
        LocalSourceRange definition_range;

        SymbolHash target_symbol;
    };
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

    /// ...
};

struct TUIndex {
    IncludeGraph graph;

    llvm::DenseMap<SymbolHash, Symbol> symbols;

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    static TUIndex build(CompilationUnit& unit);
};

}  // namespace clice::index
