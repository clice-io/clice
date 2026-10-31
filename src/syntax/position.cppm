module clice:syntax.position;

import :support.anomaly;
import :support.prelude;
import :syntax.token;

namespace clice {

using PositionEncoding = lsp::PositionEncoding;

/// Whether line `line` ends in "\r\n" by `crlf`, whose bit `n % 64` of word
/// `n / 64` marks line `n`.
inline bool ends_crlf(std::span<const std::uint64_t> crlf, std::size_t line) {
    return line / 64 < crlf.size() && ((crlf[line / 64] >> (line % 64)) & 1) != 0;
}

/// The conversions between one text's byte offsets and its positions, over
/// the line starts its owner keeps, in the encoding each call names.
/// Borrows everything.
///
/// A line ends at '\n', and a '\r' just before it belongs to the line's end,
/// not its text: an offset between them is at the line's end.
class PositionMap {
public:
    /// A text at hand and its lsp::line_starts(). Its
    /// lsp::non_ascii_lines() spare the conversions reading ASCII
    /// lines; without them every line converted is read.
    PositionMap(llvm::StringRef content,
                std::span<const std::uint32_t> lines,
                std::optional<std::span<const std::uint64_t>> non_ascii = std::nullopt) :
        content(content), content_size(static_cast<std::uint32_t>(content.size())), starts(lines),
        non_ascii(non_ascii) {}

    /// A text known by its size and line starts, with the text of the lines
    /// holding non-ASCII bytes alone: `utf8_lines` ascending, their text
    /// without the line end back to back in `utf8_text`, ending at
    /// `utf8_ends`. On every other line every encoding counts bytes. `crlf`
    /// has bit `n % 64` of word `n / 64` set when line `n` ends in "\r\n".
    PositionMap(std::uint32_t size,
                std::span<const std::uint32_t> lines,
                std::span<const std::uint64_t> crlf,
                std::span<const std::uint32_t> utf8_lines = {},
                llvm::StringRef utf8_text = {},
                std::span<const std::uint32_t> utf8_ends = {}) :
        content_size(size), starts(lines), crlf(crlf), utf8_lines(utf8_lines), utf8_text(utf8_text),
        utf8_ends(utf8_ends) {}

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
    std::optional<protocol::Position> position(std::uint32_t offset,
                                               PositionEncoding encoding) const {
        auto position = to_position(offset, encoding);
        if(!position) {
            LOG_ANOMALY(PositionMapFail, "offset {} cannot be mapped to a position", offset);
        }
        return position;
    }

    /// The positions of a byte range's ends, as position() converts each.
    std::optional<protocol::Range> range(LocalSourceRange range, PositionEncoding encoding) const {
        std::optional<protocol::Range> converted;
        if(by_line()) {
            // The start is in the text if the end is.
            if(auto end = to_position(range.end, encoding); end && range.begin <= range.end) {
                converted = protocol::Range{*to_position(range.begin, encoding), *end};
            }
        } else {
            converted = dispatch(encoding, [&](auto text, const auto&... known) {
                return lsp::to_range(text, starts, range.begin, range.end, known...);
            });
        }
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
    std::optional<std::uint32_t> offset(protocol::Position position,
                                        PositionEncoding encoding) const {
        if(auto line = line_text(position.line)) {
            auto offset = lsp::to_offset(line->text,
                                         one_line,
                                         {.line = 0, .character = position.character},
                                         encoding);
            return offset ? std::optional(line->start + *offset) : std::nullopt;
        }
        return dispatch(encoding, [&](auto text, const auto&... known) {
            return lsp::to_offset(text, starts, position, known...);
        });
    }

    /// The offset of a client's position, clamped as LSP asks: past the last
    /// line is the end of the text, past a line's end is its end, inside a
    /// code point is its start.
    std::uint32_t offset_clamped(protocol::Position position, PositionEncoding encoding) const {
        if(auto line = line_text(position.line)) {
            return line->start +
                   lsp::to_offset_clamped(line->text,
                                          one_line,
                                          {.line = 0, .character = position.character},
                                          encoding);
        }
        return dispatch(encoding, [&](auto text, const auto&... known) {
            return lsp::to_offset_clamped(text, starts, position, known...);
        });
    }

    /// The bytes a client's range covers, both ends clamped; a reversed
    /// range covers the bytes between its ends.
    LocalSourceRange offset_range(protocol::Range range, PositionEncoding encoding) const {
        if(by_line()) {
            // Clamping keeps the order of positions, so this is their order too.
            auto begin = offset_clamped(range.start, encoding);
            auto end = offset_clamped(range.end, encoding);
            return {std::min(begin, end), std::max(begin, end)};
        }
        auto offsets = dispatch(encoding, [&](auto text, const auto&... known) {
            return lsp::to_offset_range(text, starts, range, known...);
        });
        return {offsets.begin, offsets.end};
    }

    /// A line's bytes, its line end excluded; none past the last line.
    std::optional<LocalSourceRange> line_bounds(std::uint32_t line) const {
        if(line >= starts.size()) {
            return std::nullopt;
        }
        protocol::Position end{.line = line, .character = UINT32_MAX};
        // Counting bytes as ASCII finds the line's end without reading it.
        auto stop = crlf ? lsp::to_offset_clamped(content_size, starts, end, CRLF{*crlf})
                         : lsp::to_offset_clamped(std::string_view(content),
                                                  starts,
                                                  end,
                                                  PositionEncoding::UTF8,
                                                  lsp::all_ascii);
        return LocalSourceRange{starts[line], stop};
    }

private:
    /// The line table of a single line's text.
    constexpr static std::array<std::uint32_t, 1> one_line = {0};

    /// Whether conversions read the lines the map knows by line: in every
    /// encoding, since even UTF-8 places an offset inside a code point at
    /// the code point's start.
    bool by_line() const {
        return !utf8_lines.empty();
    }

    struct LineText {
        std::uint32_t start;
        std::string_view text;
    };

    /// The text of `line` when the map knows it by line.
    std::optional<LineText> line_text(std::uint32_t line) const {
        auto it = ranges::lower_bound(utf8_lines, line);
        if(it == utf8_lines.end() || *it != line) {
            return std::nullopt;
        }
        auto i = static_cast<std::size_t>(it - utf8_lines.begin());
        auto text = utf8_text.slice(i == 0 ? 0 : utf8_ends[i - 1], utf8_ends[i]);
        return LineText{starts[line], std::string_view(text.data(), text.size())};
    }

    std::optional<protocol::Position> to_position(std::uint32_t offset,
                                                  PositionEncoding encoding) const {
        if(by_line() && offset <= content_size) {
            auto number = lsp::line_of(starts, offset);
            if(auto line = line_text(number)) {
                // An offset inside the line end is at the end of the text.
                auto local = std::min<std::uint32_t>(offset - line->start, line->text.size());
                auto position = lsp::to_position(line->text, one_line, local, encoding);
                position->line = number;
                return position;
            }
        }
        return dispatch(encoding, [&](auto text, const auto&... known) {
            return lsp::to_position(text, starts, offset, known...);
        });
    }

    struct CRLF {
        std::span<const std::uint64_t> bits;

        bool operator()(std::uint32_t line) const {
            return ends_crlf(bits, line);
        }
    };

    /// Calls `convert(text, known...)` with what kotatsu's conversions take of
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
    std::span<const std::uint32_t> utf8_lines;
    llvm::StringRef utf8_text;
    std::span<const std::uint32_t> utf8_ends;
};

}  // namespace clice
