module;

#include "modules/prelude.h"

/// Positions in a text version and the sites index rows resolve to.

module clice:index.site;

import :syntax.position;
import :syntax.token;
import :vfs.file_table;

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
    std::string path;
    LocalSourceRange range;
    LineColumn begin;
    LineColumn end;
};

/// The position of an offset with both its columns, counted at once: a
/// site outlives the line table that counts them.
inline std::optional<LineColumn> line_column(const PositionMap& map, std::uint32_t offset) {
    auto bytes = map.position(offset, PositionEncoding::UTF8);
    if(!bytes) {
        return std::nullopt;
    }
    return LineColumn{
        .line = bytes->line,
        .column = bytes->character,
        .utf16_column = map.position(offset, PositionEncoding::UTF16)->character,
    };
}

}  // namespace clice::index
