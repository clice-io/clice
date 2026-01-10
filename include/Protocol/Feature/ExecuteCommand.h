#pragma once

#include "../Basic.h"

namespace clice::proto {

struct ExecuteCommandParams {
    string command;
    array<llvm::json::Value> arguments;
};

struct TextDocumentParams {
    /// The text document.
    TextDocumentIdentifier textDocument;
};

}  // namespace clice::proto
