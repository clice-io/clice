module;

#include "modules/prelude.h"

/// What the source says at a symbol: its definition and declaration text,
/// its documentation comment, and the lines its sites lie on.

module clice:index.text;

import :index.query;
import :index.site;
import :index.symbol;

namespace clice::index {

/// A symbol's definition as text: the extent's site and the text it
/// spans, sliced from the first source that holds the definition in
/// first-hit order (an open buffer's text, its preamble region, or the
/// file read from disk — verified against the index's content hash, so
/// a moved-on file degrades to no text rather than mismatched text).
struct DeclaredText {
    Site extent;
    std::string text;
};

std::optional<DeclaredText> definition_text(const IndexQuery& index, SymbolHash hash, Fid anchor);

/// The same of a declaration.
std::optional<DeclaredText> declaration_text(const IndexQuery& index, SymbolHash hash, Fid anchor);

/// The text (comment_text) of the comment documenting the symbol, read
/// as the compiled unit's doc_comment reads it: of its declarations,
/// then its definitions, in first-hit order, the first a Doxygen comment
/// documents, else the first any comment documents. With `inherit`, a
/// method overriding another without one takes that method's.
std::string doc_comment(const IndexQuery& index, SymbolHash hash, Fid anchor, bool inherit);

/// The source line each site lies on, for previews, each file read
/// once; empty when the text is unavailable (see definition_text on the
/// disk re-read).
std::vector<std::string> context_lines(const IndexQuery& index, llvm::ArrayRef<Site> sites);

/// The symbols a `@copydoc` in the comment of the symbol may name
/// (parse_copied_name), as the compiled unit's copied_doc_targets finds
/// them: from the symbol's scope outward, or in the global scope; of
/// several, the functions declaring the parameters it lists, when it
/// lists them.
std::vector<IndexQuery::Located> copied_doc_targets(const IndexQuery& index, SymbolHash hash,
                                        Fid anchor,
                                        llvm::StringRef name);

}  // namespace clice::index
