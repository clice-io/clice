module;

#include "modules/prelude.h"

module clice:feature.document_links;

import :compile.compilation;
import :index.symbol;
import :index.tu_manifest;
import :syntax.source_text;

namespace clice::feature {

/// Include-directive links of the focus file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnit& unit) -> std::vector<index::DocumentLink>;

/// Document links from manifest include edges: each edge's argument span is
/// located with find_directive_argument on its directive line. Lines the
/// manifest has no edge for (guard-skipped includes, __has_include, #embed)
/// produce no link.
auto index_document_links(SourceText& source, llvm::ArrayRef<index::IncludeEdge> edges)
    -> std::vector<index::DocumentLink>;

}  // namespace clice::feature
