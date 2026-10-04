#pragma once

/// Positions in a text version and the sites index rows resolve to.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "syntax/token.h"
#include "vfs/file_table.h"

#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/StringRef.h"

namespace clice::index {

namespace protocol = kota::ipc::protocol;

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
    std::string path;
    LocalSourceRange range;
    LineColumn begin;
    LineColumn end;
};

/// The mapping between one text's byte offsets and positions: an open
/// buffer's text and line table, or an index blob's. Blobs omit pure-ASCII
/// text — byte columns are UTF-16 columns there, so the mapping is
/// line-table arithmetic over the blob's content size, with a bit for each
/// line ending in "\r\n"; non-ASCII content counts UTF-16 units over the
/// stored text. Borrowed, never owning.
class Coordinates {
public:
    Coordinates() = default;

    /// A text at hand.
    Coordinates(llvm::StringRef content, std::span<const std::uint32_t> line_starts) :
        content(content), content_size(static_cast<std::uint32_t>(content.size())),
        starts(line_starts) {}

    /// A pure-ASCII text known by its size and line starts; `crlf_lines`
    /// has bit `n % 64` of word `n / 64` set when line `n` ends in "\r\n".
    Coordinates(std::uint32_t content_size,
                std::span<const std::uint32_t> line_starts,
                std::span<const std::uint64_t> crlf_lines) :
        content_size(content_size), starts(line_starts), crlf(crlf_lines) {}

    /// The text the offsets index, when this source stores it.
    llvm::StringRef text() const {
        return content;
    }

    /// The byte length of the indexed text, stored or not.
    std::uint32_t size() const {
        return content_size;
    }

    /// The position of a byte offset; nullopt past the text. An offset
    /// inside a line's newline is at the line's end.
    std::optional<LineColumn> position(std::uint32_t offset) const {
        if(starts.empty()) {
            return std::nullopt;
        }
        if(content.empty()) {
            auto at = kota::ipc::lsp::to_position(content_size, starts, offset, CRLFLines{crlf});
            if(!at) {
                return std::nullopt;
            }
            return LineColumn{.line = at->line, .column = at->character, .utf16_column = at->character};
        }
        auto text = std::string_view(content);
        auto at = kota::ipc::lsp::to_position(text,
                                              starts,
                                              offset,
                                              kota::ipc::lsp::PositionEncoding::UTF8);
        if(!at) {
            return std::nullopt;
        }
        auto utf16 =
            kota::ipc::lsp::encoded_length(text.substr(starts[at->line], at->character),
                                           kota::ipc::lsp::PositionEncoding::UTF16);
        return LineColumn{.line = at->line, .column = at->character, .utf16_column = utf16};
    }

    /// The byte offset of a line and UTF-16 column, as editors spell
    /// positions, a column past the line's end being its end; nullopt past
    /// the last line or inside a surrogate pair.
    std::optional<std::uint32_t> offset(std::uint32_t line, std::uint32_t utf16_column) const {
        protocol::Position position{.line = line, .character = utf16_column};
        if(content.empty()) {
            return kota::ipc::lsp::to_offset(content_size, starts, position, CRLFLines{crlf});
        }
        return kota::ipc::lsp::to_offset(std::string_view(content),
                                         starts,
                                         position,
                                         kota::ipc::lsp::PositionEncoding::UTF16);
    }

    /// A line's byte range, its newline excluded; nullopt past the last
    /// line.
    std::optional<LocalSourceRange> line_bounds(std::uint32_t line) const {
        if(line >= starts.size()) {
            return std::nullopt;
        }
        protocol::Position end{.line = line, .character = UINT32_MAX};
        if(content.empty()) {
            return LocalSourceRange{
                starts[line],
                kota::ipc::lsp::to_offset_clamped(content_size, starts, end, CRLFLines{crlf})};
        }
        // Counting bytes as ASCII finds the line's end without reading it.
        return LocalSourceRange{starts[line],
                                kota::ipc::lsp::to_offset_clamped(std::string_view(content),
                                                                  starts,
                                                                  end,
                                                                  kota::ipc::lsp::PositionEncoding::UTF8,
                                                                  kota::ipc::lsp::all_ascii)};
    }

private:
    struct CRLFLines {
        std::span<const std::uint64_t> bits;

        bool operator()(std::uint32_t line) const {
            return line / 64 < bits.size() && ((bits[line / 64] >> (line % 64)) & 1) != 0;
        }
    };

    llvm::StringRef content;
    std::uint32_t content_size = 0;
    std::span<const std::uint32_t> starts;
    std::span<const std::uint64_t> crlf;
};

}  // namespace clice::index
