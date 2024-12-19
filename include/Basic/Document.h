#pragma once

#include "Location.h"

namespace clice::proto {

struct TextDocumentItem {
    /// The text document's URI.
    DocumentUri uri;

    /// The text document's language identifier.
    string languageId;

    /// The version number of this document (it will strictly increase after each
    /// change, including undo/redo).
    uinteger version;

    /// The content of the opened text document.
    string text;
};

struct TextDocumentIdentifier {
    /// The text document's URI.
    DocumentUri uri;
};

struct VersionedTextDocumentIdentifier {
    /// The text document's URI.
    DocumentUri uri;
    /// The version number of this document.
    ///
    /// The version number of a document will increase after each change,
    /// including undo/redo. The number doesn't need to be consecutive.
    integer version;
};

struct TextDocumentContentChangeEvent {
    /// The new text of the whole document.
    string text;
};

struct TextDocumentPositionParams {
    /// The text document.
    TextDocumentIdentifier textDocument;

    /// The position inside the text document.
    Position position;
};

struct MarkupKind {
    std::string_view m_value;

    constexpr MarkupKind(std::string_view value) : m_value(value) {}

    constexpr static std::string_view PlainText = "plaintext";
    constexpr static std::string_view Markdown = "markdown";
};

struct MarkupContent {
    /// The type of the Markup.
    MarkupKind kind;

    /// The content itself.
    string value;
};

}  // namespace clice::proto
