module;

#include "support/prelude.h"

module clice:feature.document_links;

import :compile.compilation;
import :index.query;
import :index.symbol;
import :index.tu_manifest;
import :syntax.position;
import :syntax.source_text;
import :vfs.file_table;
import :vfs.ids;

namespace clice::feature {

/// Include-directive links of the focus file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnit& unit) -> std::vector<index::DocumentLink>;

/// Document links from manifest include edges: each edge's argument span is
/// located with find_directive_argument on its directive line. Lines the
/// manifest has no edge for (guard-skipped includes, __has_include, #embed)
/// produce no link.
auto document_links(SourceText& source, llvm::ArrayRef<index::IncludeEdge> edges)
    -> std::vector<index::DocumentLink>;

/// The links of `file` from its manifest's include edges, read in
/// `source`; none unless `source` is the text the file's shard indexed.
/// Unlike the entries the other results computed from the index serve,
/// the edges are disk truth: a buffer edited since (the quarantine
/// fallback, its own index current) must not get them — an edited
/// directive would get the old target, confidently wrong.
auto document_links(SourceText& source, const index::IndexQuery& query, Fid file)
    -> std::vector<index::DocumentLink>;

/// The links in LSP form, each target named the way the user knows the
/// file; a link outside `map`'s text is left out.
auto to_protocol(llvm::ArrayRef<index::DocumentLink> links,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 FileTable& files) -> std::vector<protocol::DocumentLink>;

/// The link whose argument covers `offset`; null for none.
auto link_at(llvm::ArrayRef<index::DocumentLink> links, std::uint32_t offset)
    -> const index::DocumentLink*;

/// Go-to-definition on a directive's argument: its target's file.
auto directive_location(FileTable& files, const index::DocumentLink& link) -> protocol::Location;

}  // namespace clice::feature
