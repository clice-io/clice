module;

#include "modules/prelude.h"

module clice:feature.protocol;

import :index.site;
import :index.symbol;
import :syntax.position;
import :syntax.token;
import :vfs.path;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// Render an absolute path as an LSP URI string.
///
/// On Windows the path is canonicalized first (lowercase drive, forward
/// slashes): LSP clients key documents by vscode-uri's lowercase-drive
/// form, and an uppercase-drive URI from the server never matches there.
inline auto to_uri(llvm::StringRef file) -> std::string {
    llvm::SmallString<256> storage;
    file = path::canonical(file, storage);
    auto uri = kota::ipc::lsp::URI::from_file_path(std::string_view(file.data(), file.size()));
    assert(uri && "a URI names an absolute path");
    return uri->str();
}

/// The LSP range of a site, and the location of its file.
protocol::Range to_range(const index::Site& site);
protocol::Location to_location(const index::Site& site);
std::vector<protocol::Location> to_locations(llvm::ArrayRef<index::Site> sites);
std::vector<protocol::Range> to_ranges(llvm::ArrayRef<index::Site> sites);

/// The replacements as LSP edits, all of them or none: half an edit set
/// would corrupt the buffer.
std::optional<std::vector<protocol::TextEdit>>
    to_text_edits(llvm::ArrayRef<TextReplacement> replacements,
                  const PositionMap& map,
                  PositionEncoding encoding);

/// The symbol an item's `data` carries, if intact: a prepared hierarchy
/// item's, or a completion item's whose `#include` the server adds. Items
/// carry it as a decimal string: a raw 64-bit integer would be parsed into
/// a double by a JavaScript client and come back rounded.
std::optional<index::SymbolHash> data_symbol(const std::optional<protocol::LSPAny>& data);

}  // namespace clice::feature
