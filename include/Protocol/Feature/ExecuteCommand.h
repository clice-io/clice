#pragma once

#include "../Basic.h"

namespace clice::proto {

struct ExecuteCommandParams {
    string command;
    array<any> arguments;
};

struct TextDocumentParams {
    /// The text document.
    TextDocumentIdentifier textDocument;
};

}  // namespace clice::proto
