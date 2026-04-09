#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "compile/compilation_unit.h"
#include "eventide/ipc/lsp/position.h"
#include "eventide/ipc/lsp/protocol.h"

namespace clang {

class NamedDecl;

}  // namespace clang

namespace clice::feature {

namespace protocol = eventide::ipc::protocol;

using eventide::ipc::lsp::PositionEncoding;
using eventide::ipc::lsp::PositionMapper;
using eventide::ipc::lsp::parse_position_encoding;

inline auto to_range(const PositionMapper& converter, LocalSourceRange range) -> protocol::Range {
    return protocol::Range{
        .start = *converter.to_position(range.begin),
        .end = *converter.to_position(range.end),
    };
}

struct CodeCompletionOptions {
    bool enable_keyword_snippet = false;
    bool enable_function_arguments_snippet = false;
    bool enable_template_arguments_snippet = false;
    bool insert_paren_in_function_call = false;
    bool bundle_overloads = true;
    std::uint32_t limit = 0;
};

struct HoverOptions {
    bool enable_doxygen_parsing = true;
    bool parse_comment_as_markdown = true;
    bool show_aka = true;
};

struct InlayHintsOptions {
    bool enabled = true;
    bool parameters = true;
    bool deduced_types = true;
    bool designators = true;
    bool block_end = false;
    bool default_arguments = false;
    std::uint32_t type_name_limit = 32;
};

struct SignatureHelpOptions {};

/// Implements LSP textDocument/semanticTokens.
/// Requires: AST (uses SemanticVisitor to walk declarations and a lexer for
/// keyword/literal tokens in the interested file). Combines lexical and
/// semantic token information, resolving conflicts and encoding the result
/// as delta-encoded token data per the LSP spec.
auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> protocol::SemanticTokens;

/// Implements LSP textDocument/documentLink.
/// Requires: AST (preprocessor directives — #include and __has_include).
/// Returns clickable links for each #include and __has_include that resolved
/// to a file, pointing to the target file's URI.
auto document_links(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::DocumentLink>;

/// Implements LSP textDocument/documentSymbol.
/// Requires: AST (traverses top-level and nested declarations via
/// FilteredASTVisitor). Produces a hierarchical tree of symbols (namespaces,
/// classes, functions, variables, etc.) with their ranges and detail strings.
auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::DocumentSymbol>;

/// Implements LSP textDocument/foldingRange.
/// Requires: AST (FilteredASTVisitor for code constructs) and preprocessor
/// directives (for #if/#pragma region). Produces folding ranges for
/// namespaces, classes, function bodies/params, call args, lambdas,
/// initializer lists, access specifiers, conditional directives, and regions.
auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::FoldingRange>;

/// Implements LSP textDocument/publishDiagnostics.
/// Requires: AST (collected diagnostics from Clang). Converts Clang
/// diagnostics (warnings, errors, fatals) into LSP Diagnostics with severity,
/// code, tags (deprecated/unnecessary), and related information from notes.
/// Diagnostics originating from included files are mapped to the #include line.
auto diagnostics(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::Diagnostic>;

/// Implements LSP textDocument/completion.
/// Requires: a fresh compilation (triggers Clang's code completion engine at
/// the given offset via CompilationParams). Uses fuzzy matching against the
/// typed prefix and optionally bundles function overloads into single entries.
/// @note Takes CompilationParams by reference because it drives a new parse.
auto code_complete(CompilationParams& params,
                   const CodeCompletionOptions& options = {},
                   PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::CompletionItem>;

/// Implements LSP textDocument/hover (decl-based overload).
/// Requires: AST. Produces a Markdown hover card showing the symbol kind and
/// name for the given declaration. Returns nullopt if decl is null.
auto hover(CompilationUnitRef unit,
           const clang::NamedDecl* decl,
           const HoverOptions& options = {},
           PositionEncoding encoding = PositionEncoding::UTF16) -> std::optional<protocol::Hover>;

/// Implements LSP textDocument/hover (offset-based overload).
/// Requires: AST. Uses SelectionTree to find the declaration or DeclRefExpr
/// at the given byte offset, then delegates to the decl-based overload.
/// Returns nullopt if no suitable node is found at the offset.
auto hover(CompilationUnitRef unit,
           std::uint32_t offset,
           const HoverOptions& options = {},
           PositionEncoding encoding = PositionEncoding::UTF16) -> std::optional<protocol::Hover>;

/// Implements LSP textDocument/inlayHint.
/// Requires: AST (FilteredASTVisitor to walk expressions and declarations).
/// Produces inline hints for parameter names, deduced types, aggregate
/// designators, block-end labels, and default argument values within the
/// given source range.
auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {},
                 PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::InlayHint>;

/// Implements LSP textDocument/signatureHelp.
/// Requires: a fresh compilation (triggers Clang's overload-candidate engine
/// via CompilationParams). Returns function/template/aggregate signatures
/// with labeled parameters and active-parameter tracking.
/// @note Takes CompilationParams by reference because it drives a new parse.
auto signature_help(CompilationParams& params, const SignatureHelpOptions& options = {})
    -> protocol::SignatureHelp;

/// Implements LSP textDocument/formatting and textDocument/rangeFormatting.
/// Does NOT require an AST — operates purely on file content using
/// clang-format. Detects .clang-format style, sorts includes, and reformats
/// the given range (or the entire file if range is nullopt).
auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range,
                     PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::TextEdit>;

}  // namespace clice::feature
