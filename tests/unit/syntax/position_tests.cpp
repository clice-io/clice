module;

#include "modules/prelude.h"

module clice;

import :support.anomaly;
import :syntax.position;
import :tests.unit.test.test;

namespace clice::testing {

namespace {

constexpr auto UTF8 = PositionEncoding::UTF8;
constexpr auto UTF16 = PositionEncoding::UTF16;
constexpr auto UTF32 = PositionEncoding::UTF32;

/// `line:character`, or `none` when the map has no position for the offset.
std::string at(const PositionMap& map, std::uint32_t offset, PositionEncoding encoding) {
    auto position = map.position(offset, encoding);
    return position ? std::format("{}:{}", position->line, position->character) : "none";
}

kota::ipc::protocol::Position point(std::uint32_t line, std::uint32_t character) {
    return {.line = line, .character = character};
}

ZEST_SUITE(PositionMap) {

/// "xé😀y\n": é is two bytes and one unit of either kind, 😀 four bytes, a
/// UTF-16 surrogate pair and one code point.
std::string_view content = "x\xC3\xA9\xF0\x9F\x98\x80y\n";
std::vector<std::uint32_t> lines = kota::ipc::lsp::line_starts(content);
std::vector<std::uint64_t> non_ascii = kota::ipc::lsp::non_ascii_lines(content);

ZEST_CASE(EncodingColumns) {
    PositionMap read(content, lines);
    PositionMap marked(content, lines, non_ascii);
    for(auto* map: {&read, &marked}) {
        ZEXPECT(at(*map, 7, UTF8) == "0:7");
        ZEXPECT(at(*map, 7, UTF16) == "0:4");
        ZEXPECT(at(*map, 7, UTF32) == "0:3");
        ZEXPECT(map->offset(point(0, 7), UTF8) == std::optional<std::uint32_t>(7));
        ZEXPECT(map->offset(point(0, 4), UTF16) == std::optional<std::uint32_t>(7));
        ZEXPECT(map->offset(point(0, 3), UTF32) == std::optional<std::uint32_t>(7));
        ZEXPECT(at(*map, 9, UTF16) == "1:0");
    }
}

ZEST_CASE(InsideCodePoint) {
    PositionMap map(content, lines, non_ascii);
    // UTF-16 unit 3 is the second half of 😀's surrogate pair, byte 2 the
    // second byte of é: a client position there has no offset, and clamps to
    // the code point's start.
    ZEXPECT(!map.offset(point(0, 3), UTF16));
    ZEXPECT(map.offset_clamped(point(0, 3), UTF16) == 3u);
    ZEXPECT(!map.offset(point(0, 2), UTF8));
    ZEXPECT(map.offset_clamped(point(0, 2), UTF8) == 1u);
    ZEXPECT(at(map, 5, UTF16) == "0:2");
    ZEXPECT(at(map, 2, UTF8) == "0:1");
}

ZEST_CASE(PastLineAndText) {
    PositionMap map(content, lines, non_ascii);
    ZEXPECT(map.offset(point(0, 99), UTF16) == std::optional<std::uint32_t>(8));
    ZEXPECT(map.offset_clamped(point(0, 99), UTF16) == 8u);
    ZEXPECT(!map.offset(point(2, 0), UTF16));
    ZEXPECT(map.offset_clamped(point(2, 0), UTF16) == 9u);
    ZEXPECT(map.line_bounds(0) == std::optional(LocalSourceRange{0, 8}));
    ZEXPECT(map.line_bounds(1) == std::optional(LocalSourceRange{9, 9}));
    ZEXPECT(!map.line_bounds(2));
}

ZEST_CASE(ReversedRange) {
    PositionMap map(content, lines, non_ascii);
    auto range = map.offset_range({.start = point(1, 0), .end = point(0, 4)}, UTF16);
    ZEXPECT(range == (LocalSourceRange{7, 9}));
}

/// "int a\r\n;\n": the '\r' ends line 0 with the '\n', so no position falls
/// between them and an edit past the line's end lands before the '\r'.
ZEST_CASE(CRLFLineEnd) {
    llvm::StringRef text = "int a\r\n;\n";
    auto starts = kota::ipc::lsp::line_starts(text);
    std::vector<std::uint64_t> crlf = {1};
    PositionMap read(text, starts);
    PositionMap sized(static_cast<std::uint32_t>(text.size()), starts, crlf);
    for(auto* map: {&read, &sized}) {
        ZEXPECT(at(*map, 5, UTF16) == "0:5");
        ZEXPECT(at(*map, 6, UTF16) == "0:5");
        ZEXPECT(at(*map, 7, UTF16) == "1:0");
        ZEXPECT(map->offset(point(0, 6), UTF16) == std::optional<std::uint32_t>(5));
        ZEXPECT(map->offset_clamped(point(0, 99), UTF16) == 5u);
        ZEXPECT(map->line_bounds(0) == std::optional(LocalSourceRange{0, 5}));
        ZEXPECT(map->line_bounds(1) == std::optional(LocalSourceRange{7, 8}));
    }
}

ZEST_CASE(LoneCRIsText) {
    llvm::StringRef text = "a\rb\n";
    auto starts = kota::ipc::lsp::line_starts(text);
    PositionMap map(text, starts);
    ZEXPECT(at(map, 2, UTF16) == "0:2");
    ZEXPECT(map.line_bounds(0) == std::optional(LocalSourceRange{0, 3}));
}

ZEST_CASE(SizedCountsBytes) {
    std::vector<std::uint32_t> starts = {0, 3, 6};
    PositionMap map(6, starts, {});
    ZEXPECT(map.text().empty());
    ZEXPECT(map.size() == 6u);
    for(auto encoding: {UTF8, UTF16, UTF32}) {
        ZEXPECT(at(map, 4, encoding) == "1:1");
        ZEXPECT(map.offset(point(1, 1), encoding) == std::optional<std::uint32_t>(4));
    }
    ZEXPECT(map.line_bounds(1) == std::optional(LocalSourceRange{3, 5}));
}

ZEST_CASE(PastTextAnomaly) {
    logging::reset_anomaly_for_testing();
    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    PositionMap map(content, lines, non_ascii);
    ZEXPECT(at(map, 9, UTF16) == "1:0");
    ZEXPECT(trapped.empty());
    ZEXPECT(at(map, 10, UTF16) == "none");
    ZEXPECT(!map.range({0, 10}, UTF16));
    ZEXPECT(trapped.size() == 2u);
    ZEXPECT(trapped.front() == logging::AnomalyId::PositionMapFail);

    logging::reset_anomaly_for_testing();
}

ZEST_CASE(HalfOpenRange) {
    LocalSourceRange range{2, 5};
    ZEXPECT(range.contains(2));
    ZEXPECT(!range.contains(5));
    ZEXPECT(range.touches(5));
    ZEXPECT(!range.touches(6));
}

};  // ZEST_SUITE(PositionMap)

}  // namespace

}  // namespace clice::testing
