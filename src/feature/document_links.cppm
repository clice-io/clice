module clice:feature.document_links;

import :compile.compilation;
import :index.query;
import :index.symbol;
import :index.project_index.manifest;
import :support.prelude;
import :syntax.position;
import :syntax.source_text;
import :syntax.token;
import :vfs.file_table;
import :vfs.ids;

namespace clice::feature {

/// A resolved document link: the argument range of an include-like
/// directive (byte offsets in the containing file) and the absolute path
/// of the target file. Plain data — it serializes over the worker RPC and
/// beside a preamble's PCH as-is and becomes an LSP DocumentLink only at
/// the reply edge, where the session's line map does the conversion.
struct DocumentLink {
    LocalSourceRange range;
    std::string target;
};

/// Include-directive links of the focus file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnit& unit) -> std::vector<feature::DocumentLink>;

/// Document links from the places the manifests resolved: each one's
/// argument span is located with find_directive_argument from its line and
/// column. Places no manifest resolved (guard-skipped includes) produce no
/// link.
auto document_links(SourceText& source, llvm::ArrayRef<index::ResolvedLink> resolved)
    -> std::vector<feature::DocumentLink>;

/// The links of `file` from the places the manifests resolved, read in
/// `source`; none unless `source` is the text the file's shard indexed.
/// Unlike the entries the other results computed from the index serve,
/// the links are disk truth: a buffer edited since (its compile stopped by
/// a crash, its own index current) must not get them — an edited
/// directive would get the old target, confidently wrong.
auto document_links(SourceText& source, const index::IndexQuery& query, Fid file)
    -> std::vector<feature::DocumentLink>;

/// The links the AST gives (`ast`) and those the index holds (`index`) in
/// the code the AST's context skipped (`skipped`, flat begin/end offset
/// pairs as InactiveScan's): the includes of the file's other contexts.
auto merge(std::vector<feature::DocumentLink> ast,
           llvm::ArrayRef<feature::DocumentLink> index,
           llvm::ArrayRef<std::uint32_t> skipped) -> std::vector<feature::DocumentLink>;

/// The links in LSP form, each target named the way the user knows the
/// file; a link outside `map`'s text is left out.
auto to_protocol(llvm::ArrayRef<feature::DocumentLink> links,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 FileTable& files) -> std::vector<protocol::DocumentLink>;

/// The link whose argument covers `offset`; null for none.
auto link_at(llvm::ArrayRef<feature::DocumentLink> links, std::uint32_t offset)
    -> const feature::DocumentLink*;

/// Go-to-definition on a directive's argument: its target's file.
auto directive_location(FileTable& files, const feature::DocumentLink& link) -> protocol::Location;

}  // namespace clice::feature
