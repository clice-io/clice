#pragma once

#include "Server/Protocol.h"
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
DocumentSymbols documentSymbols(ASTInfo& AST);

/// Generate document symbols for all file in AST.
index::Shared<DocumentSymbols> indexDocumentSymbols(ASTInfo& AST);

}  // namespace clice::feature

