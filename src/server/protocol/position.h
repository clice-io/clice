#pragma once

/// Positions, coordinate systems and sites shared by the master's buffer
/// access and its index projections.

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>

#include "syntax/token.h"
#include "vfs/file_table.h"

#include "kota/ipc/lsp/position.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = kota::ipc::protocol;

/// Clamp a client-supplied position to the document, following LSP
/// semantics: a character beyond the line length defaults to the line end
/// (also when it lands mid-codepoint), a line beyond the document defaults
/// to the end of the content.
inline kota::ipc::lsp::LineMap::Offset clamped_offset(const kota::ipc::lsp::LineMap& map,
                                                      const protocol::Position& position) {
    if(auto offset = map.to_offset(position)) {
        return *offset;
    }
    auto starts = map.line_starts();
    if(position.line >= starts.size()) {
        return static_cast<kota::ipc::lsp::LineMap::Offset>(map.content().size());
    }
    return map.line_bounds(starts[position.line]).end;
}

/// The coordinate system of one index source: the mapping between its
/// byte offsets and positions. Borrowed, never owning — an open buffer's
/// text and line table, or an index blob's. Blobs omit pure-ASCII text —
/// byte offsets are UTF-16 offsets there, so the mapping is line-table
/// arithmetic over the blob's content size; non-ASCII content delegates
/// to LineMap over the stored text.
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

    std::optional<protocol::Position> to_position(std::uint32_t offset) const {
        if(!content.empty()) {
            return text_map().to_position(offset);
        }
        if(offset > content_size || starts.empty()) {
            return std::nullopt;
        }
        auto line = line_of(offset);
        if(offset > line_end(line)) {
            return std::nullopt;
        }
        return protocol::Position{.line = line, .character = offset - starts[line]};
    }

    std::optional<std::uint32_t> to_offset(const protocol::Position& position) const {
        if(!content.empty()) {
            return text_map().to_offset(position);
        }
        if(position.line >= starts.size()) {
            return std::nullopt;
        }
        // Compare against the line length, not the summed offset: the
        // character is untrusted client input and the sum can wrap.
        auto start = starts[position.line];
        if(position.character > line_end(position.line) - start) {
            return std::nullopt;
        }
        return start + position.character;
    }

    std::optional<protocol::Range> to_range(std::uint32_t begin, std::uint32_t end) const {
        if(begin > end) {
            return std::nullopt;
        }
        auto start = to_position(begin);
        if(!start) {
            return std::nullopt;
        }
        auto stop = to_position(end);
        if(!stop) {
            return std::nullopt;
        }
        return protocol::Range{.start = *start, .end = *stop};
    }

private:
    kota::ipc::lsp::LineMap text_map() const {
        return kota::ipc::lsp::LineMap(std::string_view(content.data(), content.size()), starts);
    }

    std::uint32_t line_of(std::uint32_t offset) const {
        auto it = std::ranges::upper_bound(starts, offset);
        return it == starts.begin() ? 0 : static_cast<std::uint32_t>(it - starts.begin()) - 1;
    }

    /// Byte offset of the line's end, before its newline (mirroring
    /// LineMap::line_bounds).
    std::uint32_t line_end(std::uint32_t line) const {
        return line + 1 < starts.size() ? starts[line + 1] - 1 : content_size;
    }

    llvm::StringRef content;
    std::uint32_t content_size = 0;
    std::span<const std::uint32_t> starts;
};

/// One row's site: the file it lies in, its byte range in that file's
/// serving source, and the coordinates that turn the bytes into positions.
/// `file` is invalid when the path is unknown to the file table (an overlay
/// header no scan interned); `path` always names it, in the table's
/// canonical spelling whenever the file is known.
struct Site {
    Fid file;
    llvm::StringRef path;
    LocalSourceRange range;
    Coordinates coords;
};

}  // namespace clice
