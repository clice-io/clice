#include "support/position.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <utility>

namespace clice {

auto parse_position_encoding(std::string_view encoding) -> PositionEncoding {
    if(encoding == protocol::PositionEncodingKind::utf8) {
        return PositionEncoding::UTF8;
    }

    if(encoding == protocol::PositionEncodingKind::utf32) {
        return PositionEncoding::UTF32;
    }

    return PositionEncoding::UTF16;
}

auto parse_position_encoding(llvm::StringRef encoding) -> PositionEncoding {
    return parse_position_encoding(std::string_view(encoding.data(), encoding.size()));
}

PositionConverter::PositionConverter(llvm::StringRef content, PositionEncoding encoding) :
    content(content), encoding(encoding) {
    line_starts.push_back(0);
    for(std::uint32_t i = 0; i < content.size(); ++i) {
        if(content[i] == '\n') {
            line_starts.push_back(i + 1);
        }
    }
}

auto PositionConverter::line_of(std::uint32_t offset) const -> std::uint32_t {
    assert(offset <= content.size() && "offset out of range");
    auto it = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    if(it == line_starts.begin()) {
        return 0;
    }
    return static_cast<std::uint32_t>((it - line_starts.begin()) - 1);
}

auto PositionConverter::line_start(std::uint32_t line) const -> std::uint32_t {
    assert(line < line_starts.size() && "line out of range");
    return line_starts[line];
}

auto PositionConverter::line_end_exclusive(std::uint32_t line) const -> std::uint32_t {
    assert(line < line_starts.size() && "line out of range");
    if(line + 1 < line_starts.size()) {
        return line_starts[line + 1] - 1;
    }
    return static_cast<std::uint32_t>(content.size());
}

auto PositionConverter::next_codepoint_sizes(llvm::StringRef text, std::size_t index)
    -> std::pair<std::uint32_t, std::uint32_t> {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::uint32_t utf8 = 1;
    std::uint32_t utf16 = 1;

    if((lead & 0x80u) == 0u) {
        return {utf8, utf16};
    }

    if((lead & 0xE0u) == 0xC0u) {
        utf8 = 2;
    } else if((lead & 0xF0u) == 0xE0u) {
        utf8 = 3;
    } else if((lead & 0xF8u) == 0xF0u) {
        utf8 = 4;
        utf16 = 2;
    } else {
        return {1, 1};
    }

    if(index + utf8 > text.size()) {
        return {1, 1};
    }

    return {utf8, utf16};
}

auto PositionConverter::measure(llvm::StringRef text) const -> std::uint32_t {
    if(encoding == PositionEncoding::UTF8) {
        return static_cast<std::uint32_t>(text.size());
    }

    std::uint32_t units = 0;
    for(std::size_t index = 0; index < text.size();) {
        auto [utf8, utf16] = next_codepoint_sizes(text, index);
        index += utf8;
        units += (encoding == PositionEncoding::UTF16) ? utf16 : 1;
    }
    return units;
}

auto PositionConverter::character(std::uint32_t line, std::uint32_t byte_column) const
    -> std::uint32_t {
    auto start = line_start(line);
    auto end = line_end_exclusive(line);
    assert(start + byte_column <= end && "byte column out of range");
    return measure(content.substr(start, byte_column));
}

auto PositionConverter::length(std::uint32_t line,
                               std::uint32_t begin_byte_column,
                               std::uint32_t end_byte_column) const -> std::uint32_t {
    auto start = line_start(line);
    auto end = line_end_exclusive(line);
    assert(start + begin_byte_column <= end && "begin byte column out of range");
    assert(start + end_byte_column <= end && "end byte column out of range");

    if(end_byte_column <= begin_byte_column) {
        return 0;
    }

    auto size = end_byte_column - begin_byte_column;
    return measure(content.substr(start + begin_byte_column, size));
}

auto PositionConverter::to_position(std::uint32_t offset) const -> protocol::Position {
    auto line = line_of(offset);
    auto column = offset - line_start(line);
    return protocol::Position{
        .line = line,
        .character = character(line, column),
    };
}

auto PositionConverter::to_offset(protocol::Position position) const -> std::uint32_t {
    auto line = static_cast<std::uint32_t>(position.line);
    auto target = static_cast<std::uint32_t>(position.character);
    auto begin = line_start(line);
    auto end = line_end_exclusive(line);

    if(target == 0) {
        return begin;
    }

    if(encoding == PositionEncoding::UTF8) {
        assert(begin + target <= end && "character out of range");
        return begin + target;
    }

    std::uint32_t offset = begin;
    auto text = content.substr(begin, end - begin);
    for(std::size_t index = 0; index < text.size();) {
        auto [utf8, utf16] = next_codepoint_sizes(text, index);
        auto step = (encoding == PositionEncoding::UTF16) ? utf16 : 1;
        assert(target >= step && "character out of range");
        target -= step;
        offset += utf8;
        index += utf8;
        if(target == 0) {
            return offset;
        }
    }

    assert(false && "character out of range");
    return end;
}

}  // namespace clice
