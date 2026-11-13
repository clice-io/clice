/// What the source says at a symbol: its definition and declaration text,
/// and the lines its sites lie on.

module clice:index.query.text;

import :index.query;
import :index.query.site;
import :index.symbol;
import :semantic.symbol;
import :support.prelude;

namespace clice::index {

/// A symbol's definition or declaration (`kind`) as text: the extent's
/// site and the text it spans, sliced from the first source that holds
/// one, `accept`ed, in first-hit order (an open buffer's text, its preamble
/// region, or the file read from disk — verified against the index's
/// content hash, so a moved-on file degrades to no text rather than
/// mismatched text).
struct DeclaredText {
    Site extent;
    std::string text;
};

std::optional<DeclaredText> declared_text(const IndexQuery& index,
                                          SymbolHash hash,
                                          Fid anchor,
                                          RelationKind kind,
                                          llvm::function_ref<bool(llvm::StringRef)> accept = {});

/// The source line each site lies on, for previews, each file read
/// once; empty when the text is unavailable (see declared_text on the
/// disk re-read).
std::vector<std::string> context_lines(const IndexQuery& index, llvm::ArrayRef<Site> sites);

}  // namespace clice::index
