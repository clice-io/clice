#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "compile/compilation_unit.h"
#include "semantic/symbol_kind.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"

namespace clang {

class NamedDecl;

}  // namespace clang

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

using kota::ipc::lsp::PositionEncoding;
using kota::ipc::lsp::PositionMapper;
using kota::ipc::lsp::parse_position_encoding;

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

auto folding_ranges(CompilationUnitRef unit) -> std::vector<FoldingRange>;
auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::FoldingRange>;

auto document_symbols(CompilationUnitRef unit) -> std::vector<DocumentSymbol>;
auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding)
    -> std::vector<protocol::DocumentSymbol>;

auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {}) -> std::vector<InlayHint>;
auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options,
                 PositionEncoding encoding) -> std::vector<protocol::InlayHint>;

auto document_links(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::DocumentLink>;

auto diagnostics(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::Diagnostic>;

auto code_complete(CompilationParams& params,
                   const CodeCompletionOptions& options = {},
                   PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::CompletionItem>;

auto hover(CompilationUnitRef unit,
           const clang::NamedDecl* decl,
           const HoverOptions& options = {},
           PositionEncoding encoding = PositionEncoding::UTF16) -> std::optional<protocol::Hover>;

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

}  // namespace clice::feature
