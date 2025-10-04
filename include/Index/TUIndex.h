#pragma once

#include "IncludeGraph.h"
#include "AST/SourceCode.h"
#include "AST/RelationKind.h"

namespace clice::index {

using Range = LocalSourceRange;
using SymbolID2 = std::uint64_t;

struct Relation {
    RelationKind kind;

    LocalSourceRange range;

    union {
        LocalSourceRange definition_range;

        SymbolID2 target_symbol;
    };
};

struct Occurrence {
    /// range of this occurrence.
    Range range;

    ///
    SymbolID2 target;
};

struct FileIndex {
    llvm::DenseMap<SymbolID2, std::vector<Relation>> relations;

    std::vector<Occurrence> occurrences;
};

struct Symbol {
    std::string name;

    /// ...
};

struct TUIndex {
    IncludeGraph graph;

    llvm::DenseMap<SymbolID2, Symbol> symbols;

    llvm::DenseMap<clang::FileID, FileIndex> file_indices;

    static TUIndex build(CompilationUnit& unit);
};

}  // namespace clice::index
