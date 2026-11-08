module clice:feature.protocol;

import :index.site;
import :index.symbol;
import :support.prelude;
import :syntax.position;
import :syntax.token;
import :vfs.path;

namespace clice::feature {

/// Render an absolute path as an LSP URI string. On Windows the drive letter
/// is written in lower case: LSP clients key documents by vscode-uri's
/// lowercase-drive form, and an uppercase-drive URI from the server never
/// matches there.
inline auto to_uri(llvm::StringRef file) -> std::string {
    auto uri = lsp::URI::from_path(file);
    assert(uri && "a URI names an absolute path");
    return std::string(uri->text());
}

/// The file a `file:` URI names; nullopt for any other URI.
inline auto to_path(llvm::StringRef uri) -> std::optional<Spelling> {
    auto parsed = lsp::URI::from_string(uri);
    std::string path;
    if(!parsed || parsed->to_path(path) || !path::is_absolute(path)) {
        return std::nullopt;
    }
    return Spelling::absolute(path);
}

/// The LSP range of a site, and the location of its file, counted in
/// `encoding`.
protocol::Range to_range(const index::Site& site, PositionEncoding encoding);
protocol::Location to_location(const index::Site& site, PositionEncoding encoding);
std::vector<protocol::Location> to_locations(llvm::ArrayRef<index::Site> sites,
                                             PositionEncoding encoding);
std::vector<protocol::Range> to_ranges(llvm::ArrayRef<index::Site> sites,
                                       PositionEncoding encoding);

/// The replacements as LSP edits, all of them or none: half an edit set
/// would corrupt the buffer.
std::optional<std::vector<protocol::TextEdit>>
    to_protocol(llvm::ArrayRef<TextReplacement> replacements,
                const PositionMap& map,
                PositionEncoding encoding);

/// The symbol an item's `data` carries, if intact: a prepared hierarchy
/// item's, or a completion item's whose `#include` the server adds. Items
/// carry it as a decimal string: a raw 64-bit integer would be parsed into
/// a double by a JavaScript client and come back rounded.
std::optional<index::SymbolHash> data_symbol(const std::optional<protocol::LSPAny>& data);

}  // namespace clice::feature
