module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"

module clice:syntax.position;

import :support.anomaly;
import :syntax.token;

namespace clice {

using PositionEncoding = kota::ipc::lsp::PositionEncoding;

/// The conversions between one text's byte offsets and its positions, over
/// the line starts its owner keeps, in the encoding each call names.
/// Borrows everything.
///
/// A line ends at '\n', and a '\r' just before it belongs to the line's end,
/// not its text: an offset between them is at the line's end.
class PositionMap {
public:
    /// A text at hand and its kota::ipc::lsp::line_starts(). Its
    /// kota::ipc::lsp::non_ascii_lines() spare the conversions reading ASCII
    /// lines; without them every line converted is read.
    PositionMap(llvm::StringRef content,
                std::span<const std::uint32_t> lines,
                std::optional<std::span<const std::uint64_t>> non_ascii = std::nullopt) :
        content(content), content_size(static_cast<std::uint32_t>(content.size())), starts(lines),
        non_ascii(non_ascii) {}

    /// An all-ASCII text known by its size and line starts alone, where
    /// every encoding counts bytes. `crlf` has bit `n % 64` of word `n / 64`
    /// set when line `n` ends in "\r\n".
    PositionMap(std::uint32_t size,
                std::span<const std::uint32_t> lines,
                std::span<const std::uint64_t> crlf) :
        content_size(size), starts(lines), crlf(crlf) {}

    /// The text, when this map was given it.
    llvm::StringRef text() const {
        return content;
    }

    std::uint32_t size() const {
        return content_size;
    }

    /// The position of an offset the server computed: one past the text is
    /// a bug, reported as an anomaly. An offset inside a code point is at
    /// the code point's start.
    std::optional<kota::ipc::protocol::Position> position(std::uint32_t offset,
                                                          PositionEncoding encoding) const {
        auto position = dispatch(encoding, [&](auto text, const auto&... known) {
            return kota::ipc::lsp::to_position(text, starts, offset, known...);
        });
        if(!position) {
            LOG_ANOMALY(PositionMapFail, "offset {} cannot be mapped to a position", offset);
        }
        return position;
    }

    /// The positions of a byte range's ends, as position() converts each.
    std::optional<kota::ipc::protocol::Range> range(LocalSourceRange range,
                                                    PositionEncoding encoding) const {
        auto converted = dispatch(encoding, [&](auto text, const auto&... known) {
            return kota::ipc::lsp::to_range(text, starts, range.begin, range.end, known...);
        });
        if(!converted) {
            LOG_ANOMALY(PositionMapFail,
                        "range {}-{} cannot be mapped to positions",
                        range.begin,
                        range.end);
        }
        return converted;
    }

    /// The offset of a client's position, a character past the line's end
    /// being its end; none past the last line or inside a code point.
    std::optional<std::uint32_t> offset(kota::ipc::protocol::Position position,
                                        PositionEncoding encoding) const {
        return dispatch(encoding, [&](auto text, const auto&... known) {
            return kota::ipc::lsp::to_offset(text, starts, position, known...);
        });
    }

    /// The offset of a client's position, clamped as LSP asks: past the last
    /// line is the end of the text, past a line's end is its end, inside a
    /// code point is its start.
    std::uint32_t offset_clamped(kota::ipc::protocol::Position position,
                                 PositionEncoding encoding) const {
        return dispatch(encoding, [&](auto text, const auto&... known) {
            return kota::ipc::lsp::to_offset_clamped(text, starts, position, known...);
        });
    }

    /// The bytes a client's range covers, both ends clamped; a reversed
    /// range covers the bytes between its ends.
    LocalSourceRange offset_range(kota::ipc::protocol::Range range,
                                  PositionEncoding encoding) const {
        auto offsets = dispatch(encoding, [&](auto text, const auto&... known) {
            return kota::ipc::lsp::to_offset_range(text, starts, range, known...);
        });
        return {offsets.begin, offsets.end};
    }

    /// A line's bytes, its line end excluded; none past the last line.
    std::optional<LocalSourceRange> line_bounds(std::uint32_t line) const {
        if(line >= starts.size()) {
            return std::nullopt;
        }
        kota::ipc::protocol::Position end{.line = line, .character = UINT32_MAX};
        // Counting bytes as ASCII finds the line's end without reading it.
        auto stop = crlf ? kota::ipc::lsp::to_offset_clamped(content_size, starts, end, CRLF{*crlf})
                         : kota::ipc::lsp::to_offset_clamped(std::string_view(content),
                                                             starts,
                                                             end,
                                                             PositionEncoding::UTF8,
                                                             kota::ipc::lsp::all_ascii);
        return LocalSourceRange{starts[line], stop};
    }

private:
    struct CRLF {
        std::span<const std::uint64_t> bits;

        bool operator()(std::uint32_t line) const {
            return line / 64 < bits.size() && ((bits[line / 64] >> (line % 64)) & 1) != 0;
        }
    };

    /// Calls `convert(text, known...)` with what kota's conversions take of
    /// this text: a text at hand, then the encoding and what is known of its
    /// ASCII lines; or the size of one known by its size, then its "\r\n"
    /// endings.
    template <typename Convert>
    auto dispatch(PositionEncoding encoding, const Convert& convert) const
        -> std::invoke_result_t<const Convert&, std::string_view, PositionEncoding> {
        if(crlf) {
            return convert(content_size, CRLF{*crlf});
        }
        std::string_view text(content);
        if(non_ascii) {
            return convert(text, encoding, *non_ascii);
        }
        return convert(text, encoding);
    }

    llvm::StringRef content;
    std::uint32_t content_size;
    std::span<const std::uint32_t> starts;
    std::optional<std::span<const std::uint64_t>> non_ascii;
    std::optional<std::span<const std::uint64_t>> crlf;
};

}  // namespace clice
