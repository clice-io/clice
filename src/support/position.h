#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "eventide/language/protocol.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

namespace protocol = eventide::language::protocol;

enum class PositionEncoding : std::uint8_t {
    UTF8,
    UTF16,
    UTF32,
};

auto parse_position_encoding(std::string_view encoding) -> PositionEncoding;

auto parse_position_encoding(llvm::StringRef encoding) -> PositionEncoding;

class PositionConverter {
public:
    PositionConverter(llvm::StringRef content, PositionEncoding encoding);

    auto line_of(std::uint32_t offset) const -> std::uint32_t;

    auto line_start(std::uint32_t line) const -> std::uint32_t;

    auto line_end_exclusive(std::uint32_t line) const -> std::uint32_t;

    auto character(std::uint32_t line, std::uint32_t byte_column) const -> std::uint32_t;

    auto length(std::uint32_t line,
                std::uint32_t begin_byte_column,
                std::uint32_t end_byte_column) const -> std::uint32_t;

    auto to_position(std::uint32_t offset) const -> protocol::Position;

    auto to_offset(protocol::Position position) const -> std::uint32_t;

    auto measure(llvm::StringRef text) const -> std::uint32_t;

private:
    static auto next_codepoint_sizes(llvm::StringRef text, std::size_t index)
        -> std::pair<std::uint32_t, std::uint32_t>;

private:
    llvm::StringRef content;
    PositionEncoding encoding;
    std::vector<std::uint32_t> line_starts;
};

}  // namespace clice
