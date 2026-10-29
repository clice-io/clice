module clice:feature.folding_ranges;

import :compile.compilation;
import :feature.index_entries;
import :index.query;
import :semantic.symbol;
import :support.prelude;
import :syntax.position;
import :syntax.source_text;

namespace clice::feature {

/// A fold in byte offsets of the focus file; it becomes an LSP FoldingRange
/// only at the reply edge, which knows how the client folds.
struct FoldingRange {
    LocalSourceRange range;
    std::optional<protocol::FoldingRangeKind> kind;
    std::string collapsed_text;

    /// What a client folding whole lines folds, when it differs from what
    /// `range` implies: the line holding `begin` stays visible and the
    /// lines after it hide through the line holding `end`.
    std::optional<LocalSourceRange> lines;

    /// Where the text closing the fold begins: its closing delimiter, the
    /// `#endif` ending the last branch of a conditional, the
    /// `#pragma endregion` ending a region.
    std::optional<std::uint32_t> closer;
};

/// What a client does with folds.
struct FoldingClient {
    /// It folds whole lines, ignoring the character offsets.
    bool line_folding_only = false;

    /// It shows a fold's collapsed text in place of the fold.
    bool collapsed_text = false;
};

auto folding_ranges(CompilationUnit& unit) -> std::vector<FoldingRange>;

/// The kind of the fold a declaration's block makes, by the declaration's
/// kind; none for a declaration owning no block. The AST builder and the
/// index's folds share it, so a block keeps its kind when the AST
/// takes over.
auto declaration_fold_kind(SymbolKind kind) -> std::optional<protocol::FoldingRangeKind>;

/// The lines a line-folding client folds for a declaration's `block` when
/// its brace sits below `head` — the declaration's name, or the keyword
/// opening the block: from the head's line, which stays visible. Nullopt
/// when the block keeps its own lines: the brace shares the head's line,
/// the block hides nothing but its brace line, or one of the sorted
/// `block_directives` offsets lies between head and brace (the fold would
/// cut across a conditional branch's).
auto declaration_lines(llvm::StringRef content,
                       LocalSourceRange block,
                       std::uint32_t head,
                       llvm::ArrayRef<std::uint32_t> block_directives)
    -> std::optional<LocalSourceRange>;

/// Protocol encoding of computed folds of `content`, for the worker's AST
/// results and the master's results from the index alike. A client folding
/// whole lines gets one fold per set of hidden lines; when it also shows
/// collapsed text, that text is the fold's whole first line, and a fold
/// whose closer sits on a line of its own folds that line too
/// (`int f() {...}`).
auto to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                 llvm::StringRef content,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 const FoldingClient& client) -> std::vector<protocol::FoldingRange>;

/// Declaration folds: each definition extent folds its last balanced brace
/// group (brace matching over a raw lex, so braces in strings, comments
/// and directive lines do not count), keeping the name and signature
/// visible like the AST folds do. Extents touching a conditional whose
/// branches unbalance braces produce no fold — a raw pairing there is
/// wrong for some preprocessing variant.
auto folding_ranges(SourceText& source,
                    llvm::ArrayRef<IndexDecl> decls,
                    IndexSymbolResolver resolve) -> std::vector<FoldingRange>;

/// The same from the index serving the file (see semantic_tokens).
auto folding_ranges(SourceText& source,
                    const index::IndexQuery& query,
                    const index::IndexSource& index) -> std::vector<FoldingRange>;

}  // namespace clice::feature
