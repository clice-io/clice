#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clice::proto {

using integer = std::int32_t;
/// range in [0, 2^31- 1]
using uinteger = std::uint32_t;
using decimal = double;

using string = std::string;

template <typename T>
using array = std::vector<T>;

template <typename T>
using optional = std::optional<T>;

using PositionEncodingKind = string;

struct WorkDoneProgressOptions {
    bool workDoneProgress;
};

using URI = string;
using DocumentUri = string;

enum class ErrorCodes : integer {
    /// Defined by JSON-RPC.
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    /// JSON-RPC error code indicating a server error.
    serverErrorStart = -32099,
    serverErrorEnd = -32000,
    ServerNotInitialized = -32002,
    UnknownErrorCode = -32001,

    /// Defined by the protocol.
    RequestFailed = -32803,
    ServerCancelled = -32802,
    ContentModified = -32801,
    RequestCancelled = -32800
};

struct Position {
    /// Line position in a document (zero-based).
    uinteger line;

    /// Character offset on a line in a document (zero-based).
    /// The meaning of this offset is determined by the negotiated
    /// `PositionEncodingKind`.
    uinteger character;

    constexpr friend bool operator==(const Position&, const Position&) = default;
};

struct Range {
    /// The range's start position.
    Position start;

    /// The range's end position.
    Position end;

    constexpr friend bool operator==(const Range&, const Range&) = default;
};

struct Location {
    DocumentUri uri;

    Range range;
};

struct TextEdit {
    /// The range of the text document to be manipulated. To insert
    /// text into a document create a range where start === end.
    Range range;

    // The string to be inserted. For delete operations use an
    // empty string.
    string newText;
};

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

    /// The version of document.
    integer version;
};

struct TextDocumentPositionParams {
    /// The text document.
    TextDocumentIdentifier textDocument;

    /// The position inside the text document.
    Position position;
};

struct MarkupContent {
    /// The type of the Markup.
    string kind;

    /// The content itself.
    string value;
};

}  // namespace clice::proto
