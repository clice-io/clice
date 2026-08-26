#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "compile/compilation_unit.h"
#include "index/types.h"
#include "semantic/display.h"
#include "semantic/symbol.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/markup.h"

#include "kota/codec/macro.h"
#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"
#include "kota/meta/annotation.h"
#include "llvm/ADT/ArrayRef.h"

namespace clice::feature {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

// Feature options double as their clice.toml/initializationOptions config
// sections: `defaulted = true` lets a decode leave unmentioned fields at
// their initializers, so those are the single source of every default and a
// config source only ever overlays what it names.

using kota::ipc::lsp::LineMap;
using kota::ipc::lsp::PositionEncoding;

/// Render a file path (or an already-formed URI) as an LSP URI string.
///
/// On Windows the path is canonicalized first (lowercase drive, forward
/// slashes): clang reports whatever spelling the -I dirs and CDB used,
/// while LSP clients key documents by vscode-uri's lowercase-drive form
/// — an uppercase-drive URI from the server never matches there.
inline auto to_uri(llvm::StringRef file) -> std::string {
    llvm::SmallString<256> storage;
    file = path::canonical(file, storage);
    const auto file_view = std::string_view(file.data(), file.size());

    // Convert as a path first: a Windows drive prefix like "f:" would
    // otherwise be accepted by URI::parse as a single-letter scheme.
    if(auto uri = kota::ipc::lsp::URI::from_file_path(file_view)) {
        return uri->str();
    }

    if(auto parsed = kota::ipc::lsp::URI::parse(file_view)) {
        auto str = parsed->str();
#ifdef _WIN32
        // An already-formed file URI can carry an uppercase drive;
        // canonicalize it like the path branch would have.
        constexpr std::size_t at = sizeof("file:///") - 1;
        if(str.size() > at + 1 && llvm::StringRef(str).starts_with("file:///") &&
           llvm::isUpper(str[at]) && str[at + 1] == ':') {
            str[at] = llvm::toLower(str[at]);
        }
#endif
        return str;
    }

    return file.str();
}

inline auto to_position(const LineMap& map, std::uint32_t offset)
    -> std::optional<protocol::Position> {
    if(auto position = map.to_position(offset)) {
        return *position;
    }
    LOG_ANOMALY(PositionMapFail, "offset {} cannot be mapped to a position", offset);
    return std::nullopt;
}

inline auto to_range(const LineMap& map, LocalSourceRange range) -> std::optional<protocol::Range> {
    auto start = to_position(map, range.begin);
    auto end = to_position(map, range.end);
    if(!start || !end)
        return std::nullopt;
    return protocol::Range{.start = *start, .end = *end};
}

/// Corresponds to the `[code_completion]` section in clice.toml.
struct CodeCompletionOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Complete keywords as snippets (not yet implemented).")
    <bool> enable_keyword_snippet = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Insert function arguments as a snippet when completing "
                         "a call. For functions this applies to individually "
                         "listed overloads, so it requires `bundle_overloads = "
                         "false`; function-like macros have no overload sets and "
                         "always take the snippet.")
    <bool> enable_function_arguments_snippet = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Insert template arguments as a snippet on completion "
                         "(not yet implemented).")
    <bool> enable_template_arguments_snippet = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Insert parentheses when completing a function call "
                         "(not yet implemented).")
    <bool> insert_paren_in_function_call = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Collapse an overload set into a single completion item.")
    <bool> bundle_overloads = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Maximum number of completion items (not yet implemented).")
    <std::uint32_t> limit = 0;
};

/// Corresponds to the `[hover]` section in clice.toml.
struct HoverOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Render the hover card as markdown; `false` produces "
                         "plain text for clients that cannot display it.")
    <bool> parse_comment_as_markdown = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Show the desugared form of a type, e.g. "
                         "`vector<int>::size_type (aka unsigned long)`.")
    <bool> show_aka = true;
};

/// Contains detailed information about a symbol. Especially useful when
/// generating hover responses. It can be rendered as a hover panel, or
/// embedding clients can use the structured information to provide their own
/// UI.
struct HoverInfo {
    struct PassType {
        /// How the argument is passed to the callee.
        enum class PassMode : std::uint8_t {
            Ref,
            ConstRef,
            Value,
        };

        bool operator==(const PassType&) const = default;

        PassMode pass_by = PassMode::Ref;

        /// True if implicit type conversion happened. This includes calls to
        /// implicit constructor, as well as built-in type conversions. Casting
        /// to base class is not considered conversion.
        bool converted = false;
    };

    using PassMode = PassType::PassMode;

    /// For a variable named Bar, declared in clice::feature::foo the
    /// following fields will hold:
    /// - namespace_scope: clice::feature::
    /// - local_scope: foo::
    /// - name: Bar
    ///
    /// Scopes might be None in cases where they don't make sense, e.g.
    /// auto/decltype. Contains all of the enclosing namespaces, empty string
    /// means global namespace.
    std::optional<std::string> namespace_scope;

    /// Remaining named contexts in the symbol's qualified name, empty string
    /// means the symbol is not local.
    std::string local_scope;

    /// Name of the symbol, does not contain any "::".
    std::string name;

    /// The range of the symbol in the interested file, used by the client to
    /// highlight the hovered token.
    std::optional<LocalSourceRange> symbol_range;

    SymbolKind kind = SymbolKind::Invalid;

    std::string documentation;

    /// Source code containing the definition of the symbol.
    std::string definition;

    /// Access specifier for declarations inside class/struct/unions, empty
    /// for others.
    std::string access_specifier;

    /// Printable variable type. Set only for variables.
    std::optional<display::Type> type;

    /// Set for functions and lambdas.
    std::optional<display::Type> return_type;

    /// Set for functions and lambdas with parameters.
    std::optional<std::vector<display::Param>> parameters;

    /// Set for all templates (function, class, variable).
    std::optional<std::vector<display::Param>> template_parameters;

    /// Contains the evaluated value of the symbol if available.
    std::optional<std::string> value;

    /// Contains the bit-size of fields and types where it's interesting.
    std::optional<std::uint64_t> size;

    /// Contains the offset of fields within the enclosing class.
    std::optional<std::uint64_t> offset;

    /// Contains the padding following a field within the enclosing class.
    std::optional<std::uint64_t> padding;

    /// Contains the alignment of fields and types where it's interesting.
    std::optional<std::uint64_t> align;

    /// Set when the symbol is inside a function call. Contains information
    /// extracted from the callee definition about the argument this is
    /// passed as.
    std::optional<display::Param> callee_arg_info;

    /// Set only if callee_arg_info is set.
    std::optional<PassType> call_pass_type;

    /// Produce a user-readable information.
    markup::Document present() const;
};

/// Try to infer structure of a documentation comment (e.g. line breaks).
void parse_documentation(llvm::StringRef input, markup::Document& output);

/// Corresponds to the `[inlay_hints]` section in clice.toml.
struct InlayHintsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Master switch: `false` disables all inlay hints.")
    <bool> enabled = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Parameter name hints at call sites, e.g. `draw(width: "
                         "800, height: 600)`, including `&` markers for arguments "
                         "passed by mutable reference.")
    <bool> parameters = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Deduced type hints for `auto` variables, structured "
                         "bindings and deduced return types.")
    <bool> deduced_types = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Field designator hints in aggregate initialization, "
                         "e.g. `.x=` and `.y=` in `Point{1, 2}`.")
    <bool> designators = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "`// name` hints after the closing brace of long blocks "
                         "(functions, types, namespaces, control flow).")
    <bool> block_end = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Show the default arguments a call omitted, abbreviated "
                         "when long.")
    <bool> default_arguments = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Byte budget for rendered hint text: over-long deduced "
                         "types fall back to a sugared spelling or are dropped, "
                         "over-long default arguments are abbreviated. `0` means "
                         "no limit.")
    <std::uint32_t> type_name_limit = 32;
};

struct SignatureHelpOptions {};

struct SemanticToken {
    LocalSourceRange range;
    SymbolKind kind = SymbolKind::Invalid;
    std::uint32_t modifiers = 0;
};

struct FoldingRange {
    LocalSourceRange range;
    std::optional<protocol::FoldingRangeKind> kind;
    std::string collapsed_text;
};

/// A resolved document link: the argument range of an include-like
/// directive (byte offsets in the containing file) and the absolute path
/// of the target file. Plain data — it serializes over the worker RPC and
/// the PCH's pch.idx envelope as-is and becomes an LSP DocumentLink only
/// at the reply edge, where the session's line map does the conversion.
struct DocumentLink {
    LocalSourceRange range;
    std::string target;
};

/// Result of scanning a unit for preprocessor-inactive regions.
struct InactiveScan {
    /// Byte-offset ranges [begin0, end0, begin1, end1, ...] of inactive
    /// branch bodies in the interested file; directive lines excluded.
    std::vector<std::uint32_t> regions;

    /// Conditional levels still open at the end of the scanned content,
    /// outermost first. Bit 0: the level's current branch is inactive.
    /// Bit 1: an earlier branch of the level was already taken (decides a
    /// later #else). Preamble/PCH builds end mid-#if when the bound cuts
    /// inside a block; the AST compile resumes from this stack.
    std::vector<std::uint8_t> open_stack;
};

struct DocumentSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Invalid;
    LocalSourceRange range;
    LocalSourceRange selection_range;
    std::vector<DocumentSymbol> children;
};

enum class HintCategory : std::uint8_t {
    Parameter,
    DefaultArgument,
    Type,
    Designator,
    BlockEnd,
};

struct InlayHint {
    std::uint32_t offset = 0;
    HintCategory kind = HintCategory::Type;
    std::string label;
    bool padding_left = false;
    bool padding_right = false;
};

auto semantic_tokens(CompilationUnitRef unit) -> std::vector<SemanticToken>;
auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding)
    -> protocol::SemanticTokens;

/// Wire encoding of computed tokens against the text they describe — one
/// encoder for the worker's AST results and the master's index
/// projections, so both paths emit byte-identical replies.
auto semantic_tokens_to_protocol(llvm::ArrayRef<SemanticToken> tokens,
                                 llvm::StringRef content,
                                 llvm::ArrayRef<std::uint32_t> line_starts,
                                 PositionEncoding encoding) -> protocol::SemanticTokens;

auto folding_ranges(CompilationUnitRef unit) -> std::vector<FoldingRange>;
auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::FoldingRange>;

auto folding_ranges_to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                                llvm::StringRef content,
                                llvm::ArrayRef<std::uint32_t> line_starts,
                                PositionEncoding encoding) -> std::vector<protocol::FoldingRange>;

auto document_symbols(CompilationUnitRef unit) -> std::vector<DocumentSymbol>;
auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol>;

auto document_symbols_to_protocol(llvm::ArrayRef<DocumentSymbol> symbols,
                                  llvm::StringRef content,
                                  llvm::ArrayRef<std::uint32_t> line_starts,
                                  PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol>;

auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {}) -> std::vector<InlayHint>;
auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options,
                 PositionEncoding encoding) -> std::vector<protocol::InlayHint>;

/// Include-directive links of the interested file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnitRef unit) -> std::vector<DocumentLink>;

/// Find the filename-like argument of a preprocessor directive on the line
/// containing `offset`. The offset may point at the directive/operator or
/// inside its argument.
auto find_directive_argument(llvm::StringRef content,
                             std::uint32_t offset,
                             const clang::LangOptions* lang_opts)
    -> std::optional<LocalSourceRange>;

/// Go-to-definition on an include directive: when `offset` falls on the
/// argument of an #include or __has_include in the interested file, the
/// resolved file's location (at its start). Empty otherwise.
auto include_definition(CompilationUnitRef unit, std::uint32_t offset)
    -> std::vector<protocol::Location>;

/// Scan the interested file's condition directives. `open_stack` seeds the
/// nesting state (from a preceding preamble scan) and `resume_offset` is
/// where the scanned content starts — pending inactive levels from the
/// seed begin there. A scan that ends with open levels closes their
/// pending regions at `end_offset` (the content bound).
InactiveScan inactive_regions(CompilationUnitRef unit,
                              llvm::ArrayRef<std::uint8_t> open_stack = {},
                              std::uint32_t resume_offset = 0,
                              std::uint32_t end_offset = UINT32_MAX);

auto diagnostics(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::Diagnostic>;

auto code_complete(CompilationParams& params,
                   const CodeCompletionOptions& options = {},
                   PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::CompletionItem>;

/// Get the hover information for the symbol at the given offset in the
/// interested file of the unit.
auto hover_info(CompilationUnitRef unit, std::uint32_t offset, const HoverOptions& options = {})
    -> std::optional<HoverInfo>;

/// Render structured hover information with the configured markup format and
/// convert its byte range through the caller's current line map.
auto to_protocol_hover(const HoverInfo& info, const HoverOptions& options, const LineMap& map)
    -> protocol::Hover;

auto hover(CompilationUnitRef unit,
           std::uint32_t offset,
           const HoverOptions& options = {},
           PositionEncoding encoding = PositionEncoding::UTF16) -> std::optional<protocol::Hover>;

auto signature_help(CompilationParams& params, const SignatureHelpOptions& options = {})
    -> protocol::SignatureHelp;

auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range,
                     PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::TextEdit>;

/// Index projections: whole-document features computed from index rows plus
/// the document text, serving open files that have no AST yet (see
/// FeatureRouter's routing rules). Inputs are index vocabulary types and
/// narrow resolvers, never index storage — the caller extracts rows from
/// shard/ProjectIndex and hands them over. Each projection's output is an
/// honest subset of its AST twin's: missing pieces (Sema modifiers, outline
/// detail, statement folds, ...) are pinned by the read-only test corpus.

/// One Decl/Def relation row of the document: the name-token anchor, the
/// declaration's full extent (invalid when the index recorded none — a
/// cross-file or invalid source range) and the symbol it belongs to.
struct IndexDeclRow {
    LocalSourceRange range;
    LocalSourceRange extent;
    index::SymbolHash symbol = 0;
    bool definition = false;
};

/// An include edge of the document, from the TU manifest: the 1-based
/// directive line and the resolved target's absolute path.
struct IndexIncludeEdge {
    std::uint32_t line = 0;
    std::string target;
};

/// A row symbol's identity as the resolver hands it back.
struct IndexSymbolInfo {
    std::string name;
    SymbolKind kind = SymbolKind::Invalid;
};

using IndexSymbolResolver = llvm::function_ref<std::optional<IndexSymbolInfo>(index::SymbolHash)>;

/// Language options for raw-lexing `path` without a compile command: C
/// for .c files and when `c_rows` says the served rows were built by C
/// parses only, C++ otherwise. A default-constructed LangOptions is C89
/// — `class` would lex as an identifier. `standard` is the serving
/// command's -std value when known: the rows were classified under it,
/// and a newer standard's extra keywords (`concept` in C++17 code)
/// would shadow them. Absent, unknown or contradicting values fall back
/// to the driver's default standard — the dialect a -std-less command
/// was indexed under.
auto index_lang_options(llvm::StringRef path, bool c_rows, llvm::StringRef standard = {})
    -> const clang::LangOptions&;

/// Lexical layer (keywords, literals, comments, directives) from a raw lex
/// of `content`, semantic kinds from `occurrences` resolved through
/// `resolve`, Declaration/Definition modifiers from `decls`. Both row
/// arrays must be sorted by range, as shard readers hand them out.
auto index_semantic_tokens(llvm::StringRef content,
                           const clang::LangOptions& lang_opts,
                           llvm::ArrayRef<index::Occurrence> occurrences,
                           llvm::ArrayRef<IndexDeclRow> decls,
                           IndexSymbolResolver resolve) -> std::vector<SemanticToken>;

/// Outline tree built from declaration extents by range containment;
/// extents that merely overlap (macro-generated siblings collapse onto one
/// invocation range) become siblings in source order. `detail` stays empty.
auto index_document_symbols(llvm::ArrayRef<IndexDeclRow> decls, IndexSymbolResolver resolve)
    -> std::vector<DocumentSymbol>;

/// Declaration folds: each definition extent folds its last balanced brace
/// group (brace matching over a raw lex, so braces in strings, comments
/// and directive lines do not count), keeping the name and signature
/// visible like the AST folds do. Extents touching a conditional whose
/// branches unbalance braces produce no fold — a raw pairing there is
/// wrong for some preprocessing variant.
auto index_folding_ranges(llvm::StringRef content,
                          const clang::LangOptions& lang_opts,
                          llvm::ArrayRef<IndexDeclRow> decls,
                          IndexSymbolResolver resolve) -> std::vector<FoldingRange>;

/// Document links from manifest include edges: each edge's argument span is
/// located with find_directive_argument on its directive line. Lines the
/// manifest has no edge for (guard-skipped includes, __has_include, #embed)
/// produce no link.
auto index_document_links(llvm::StringRef content,
                          const clang::LangOptions& lang_opts,
                          llvm::ArrayRef<IndexIncludeEdge> edges) -> std::vector<DocumentLink>;

/// The comment block immediately preceding the line containing `offset`:
/// contiguous //- or /*-style lines directly above it, comment markers
/// stripped. Empty when a blank line or code intervenes. An approximation
/// of clang's comment attachment, pinned as such by the read-only corpus.
auto preceding_comment(llvm::StringRef content, std::uint32_t offset) -> std::string;

/// Assemble the read-only hover card: name, kind and what the index can
/// prove from stored text — no Sema products (type, value, size, aka) and
/// no qualified scope (the index stores unqualified names).
auto index_hover(const IndexSymbolInfo& info,
                 llvm::StringRef definition_text,
                 llvm::StringRef comment) -> HoverInfo;

}  // namespace clice::feature
