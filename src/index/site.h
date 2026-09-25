#pragma once

/// Positions in a text version and the sites index rows resolve to.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "syntax/token.h"
#include "vfs/file_table.h"

#include "kota/ipc/lsp/text.h"
#include "llvm/ADT/StringRef.h"

namespace clice::index {

/// A position in a text: its 0-based line, and its column from the line
/// start counted in bytes and in UTF-16 code units — the same number for
/// ASCII text, and what editors count.
struct LineColumn {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::uint32_t utf16_column = 0;
};

/// One row's site: the file, the row's byte range in the text the rows
/// were built from, and the range's ends as positions. `path` names the
/// file as the user knows it (FileTable::display).
struct Site {
    Fid file;
    llvm::StringRef path;
    LocalSourceRange range;
    LineColumn begin;
    LineColumn end;
};

/// The mapping between one text's byte offsets and positions: an open
/// buffer's text and line table, or an index blob's. Blobs omit pure-ASCII
/// text — byte columns are UTF-16 columns there, so the mapping is
/// line-table arithmetic over the blob's content size; non-ASCII content
/// counts UTF-16 units over the stored text. Borrowed, never owning.
class Coordinates {
public:
    Coordinates() = default;

    Coordinates(llvm::StringRef content,
                std::uint32_t content_size,
                std::span<const std::uint32_t> line_starts) :
        content(content), content_size(content_size), starts(line_starts) {}

    /// The text the offsets index, when this source stores it.
    llvm::StringRef text() const {
        return content;
    }

    /// The byte length of the indexed text, stored or not.
    std::uint32_t size() const {
        return content_size;
    }

    /// The position of a byte offset; nullopt past the text or inside a
    /// line's newline.
    std::optional<LineColumn> position(std::uint32_t offset) const {
        if(offset > content_size || starts.empty()) {
            return std::nullopt;
        }
        auto line = line_of(offset);
        if(offset > line_end(line)) {
            return std::nullopt;
        }
        auto column = offset - starts[line];
        auto utf16 = content.empty() ? column
                                     : kota::ipc::lsp::encoded_length(
                                           std::string_view(content.data() + starts[line], column),
                                           kota::ipc::lsp::PositionEncoding::UTF16);
        return LineColumn{.line = line, .column = column, .utf16_column = utf16};
    }

    /// The byte offset of a line and UTF-16 column, as editors spell
    /// positions; nullopt outside the text.
    std::optional<std::uint32_t> offset(std::uint32_t line, std::uint32_t utf16_column) const {
        auto bounds = line_bounds(line);
        if(!bounds) {
            return std::nullopt;
        }
        if(content.empty()) {
            // Compare against the line length, not the summed offset: the
            // column is untrusted client input and the sum can wrap.
            if(utf16_column > bounds->length()) {
                return std::nullopt;
            }
            return bounds->begin + utf16_column;
        }
        auto within = kota::ipc::lsp::encoded_offset(
            std::string_view(content.data() + bounds->begin, bounds->length()),
            utf16_column,
            kota::ipc::lsp::PositionEncoding::UTF16);
        if(!within) {
            return std::nullopt;
        }
        return bounds->begin + *within;
    }

    /// A line's byte range, its newline excluded; nullopt past the last
    /// line.
    std::optional<LocalSourceRange> line_bounds(std::uint32_t line) const {
        if(line >= starts.size()) {
            return std::nullopt;
        }
        return LocalSourceRange{starts[line], line_end(line)};
    }

private:
    std::uint32_t line_of(std::uint32_t offset) const {
        auto it = std::ranges::upper_bound(starts, offset);
        return it == starts.begin() ? 0 : static_cast<std::uint32_t>(it - starts.begin()) - 1;
    }

    std::uint32_t line_end(std::uint32_t line) const {
        return line + 1 < starts.size() ? starts[line + 1] - 1 : content_size;
    }

    llvm::StringRef content;
    std::uint32_t content_size = 0;
    std::span<const std::uint32_t> starts;
};

}  // namespace clice::index
