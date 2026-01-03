#pragma once

#include "AST/SourceCode.h"
#include "AST/SymbolKind.h"
#include "Index/Shared.h"

namespace clice::feature {

struct DocumentSymbol {
    /// The range of symbol name in source code.
    LocalSourceRange selectionRange;

    /// The range of whole symbol.
    LocalSourceRange range;

    /// The symbol kind of this document symbol.
    SymbolKind kind;

    /// The symbol name.
    std::string name;

    /// Extra information about this symbol.
    std::string detail;

    /// The symbols that this symbol contains
    std::vector<DocumentSymbol> children;
};

using DocumentSymbols = std::vector<DocumentSymbol>;

/// Generate document symbols for only interested file.
DocumentSymbols document_symbols(CompilationUnitRef unit);

/// Generate document symbols for all file in unit.
index::Shared<DocumentSymbols> index_document_symbol(CompilationUnitRef unit);

}  // namespace clice::feature
