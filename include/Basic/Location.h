#pragma once

#include "Basic.h"
#include "Support/Enum.h"

namespace clice::proto {

/// A set of predefined position encoding kinds.
struct PositionEncodingKind : refl::Enum<PositionEncodingKind, false, std::string_view> {
    using Enum::Enum;

    constexpr inline static std::string_view UTF8 = "utf-8";
    constexpr inline static std::string_view UTF16 = "utf-16";
    constexpr inline static std::string_view UTF32 = "utf-32";

    constexpr inline static std::array All = {UTF8, UTF16, UTF32};
};

struct Position {
    /// Line position in a document (zero-based).
    uinteger line;

    /// Character offset on a line in a document (zero-based).
    /// The meaning of this offset is determined by the negotiated `PositionEncodingKind`.
    uinteger character;
};

constexpr bool operator== (const proto::Position& lhs, const proto::Position rhs) {
    return lhs.character == rhs.character && lhs.line == rhs.line;
}

constexpr auto operator<=> (const proto::Position& lhs, const proto::Position rhs) {
    return std::tie(lhs.line, lhs.character) <=> std::tie(rhs.line, rhs.character);
}

struct Range {
    /// The range's start position.
    Position start;

    /// The range's end position.
    Position end;
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

}  // namespace clice::proto

