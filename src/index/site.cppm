module;

#include "modules/prelude.h"

/// Positions in a text version and the sites index entries resolve to.

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

    /// The position as a client counting in `encoding` takes it.
    kota::ipc::protocol::Position in(PositionEncoding encoding) const {
        switch(encoding) {
            case PositionEncoding::UTF8: return {.line = line, .character = column};
            case PositionEncoding::UTF16: return {.line = line, .character = utf16_column};
            // No client is offered UTF-32.
            case PositionEncoding::UTF32: break;
        }
        std::unreachable();
    }
};

/// What a site's entry comes from.
enum class SiteOrigin : std::uint8_t {
    /// The code as written.
    Written,
    /// An instantiation of the template the site lies in.
    Instantiation,
    /// An expansion of the macro whose body the site lies in.
    Expansion,
};

/// One entry's site: the file, the entry's byte range in the text the index
/// was built from, and the range's ends as positions. `path` names the
/// file as the user knows it (FileTable::display).
struct Site {
    Fid file;
    std::string path;
    LocalSourceRange range;
    LineColumn begin;
    LineColumn end;

    SiteOrigin origin = SiteOrigin::Written;
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

/// The line holding `offset` in the text `positions` maps, which it must
/// have been given, its line end excluded.
inline llvm::StringRef line_at(const PositionMap& positions, std::uint32_t offset) {
    auto line = positions.position(offset, PositionEncoding::UTF8)->line;
    auto bounds = *positions.line_bounds(line);
    return positions.text().slice(bounds.begin, bounds.end);
}

/// The site of `range` in the text `positions` maps; nullopt for a range
/// past the text.
inline std::optional<Site> site_of(Fid file,
                                   llvm::StringRef path,
                                   const PositionMap& positions,
                                   LocalSourceRange range,
                                   SiteOrigin origin = SiteOrigin::Written) {
    if(range.end > positions.size()) {
        return std::nullopt;
    }
    return Site{
        .file = file,
        .path = path.str(),
        .range = range,
        .begin = *line_column(positions, range.begin),
        .end = *line_column(positions, range.end),
        .origin = origin,
    };
}

}  // namespace clice::index
