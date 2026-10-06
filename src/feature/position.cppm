module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"

module clice:feature.position;

import :support.anomaly;
import :syntax.token;

namespace clice::feature {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

using kota::ipc::lsp::PositionEncoding;

namespace detail {

/// Calls `convert` with what is known of the ASCII lines: the bitmap, or
/// nothing, which reads every line.
template <typename Convert>
auto with_ascii(std::optional<std::span<const std::uint64_t>> non_ascii, const Convert& convert) {
    if(non_ascii) {
        return convert(*non_ascii);
    }
    return convert([](std::uint32_t) { return false; });
}

}  // namespace detail

/// The conversions between one text's byte offsets and its positions in one
/// encoding, over the line starts its owner keeps. Borrows everything.
struct PositionMap {
    std::string_view content;

    /// lsp::line_starts() of `content`.
    std::span<const std::uint32_t> lines;

    /// lsp::non_ascii_lines() of `content`, which spares the conversions
    /// reading ASCII lines; without it every line converted is read. An
    /// empty bitmap says every line is ASCII.
    std::optional<std::span<const std::uint64_t>> non_ascii;

    PositionEncoding encoding = PositionEncoding::UTF16;

    /// The position of an offset the server computed: one past the text is
    /// a bug, reported as an anomaly.
    std::optional<protocol::Position> to_position(std::uint32_t offset) const {
        auto position = detail::with_ascii(non_ascii, [&](const auto& ascii) {
            return lsp::to_position(content, lines, offset, encoding, ascii);
        });
        if(!position) {
            LOG_ANOMALY(PositionMapFail, "offset {} cannot be mapped to a position", offset);
        }
        return position;
    }

    std::optional<protocol::Range> to_range(LocalSourceRange range) const {
        auto start = to_position(range.begin);
        auto end = to_position(range.end);
        if(!start || !end) {
            return std::nullopt;
        }
        return protocol::Range{.start = *start, .end = *end};
    }

    /// The offset of a client's position; none past the last line or inside
    /// a code point.
    std::optional<std::uint32_t> to_offset(protocol::Position position) const {
        return detail::with_ascii(non_ascii, [&](const auto& ascii) {
            return lsp::to_offset(content, lines, position, encoding, ascii);
        });
    }

    /// The offset of a client's position, clamped as LSP asks: past the last
    /// line is the end of the text, past a line's end is its end, inside a
    /// code point is its start.
    std::uint32_t to_offset_clamped(protocol::Position position) const {
        return detail::with_ascii(non_ascii, [&](const auto& ascii) {
            return lsp::to_offset_clamped(content, lines, position, encoding, ascii);
        });
    }

    /// The bytes a client's range covers, both ends clamped; a reversed
    /// range covers the bytes between its ends.
    LocalSourceRange to_offset_range(protocol::Range range) const {
        auto offsets = detail::with_ascii(non_ascii, [&](const auto& ascii) {
            return lsp::to_offset_range(content, lines, range, encoding, ascii);
        });
        return {offsets.begin, offsets.end};
    }
};

}  // namespace clice::feature
