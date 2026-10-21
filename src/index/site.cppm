module;

#include "modules/prelude.h"

/// The sites index entries resolve to.

module clice:index.site;

import :syntax.position;
import :syntax.token;
import :vfs.file_table;

namespace clice::index {

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
        .begin = *positions.line_column(range.begin),
        .end = *positions.line_column(range.end),
        .origin = origin,
    };
}

}  // namespace clice::index
