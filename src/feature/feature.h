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

auto semantic_tokens(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> protocol::SemanticTokens;

auto document_links(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::DocumentLink>;

auto document_symbols(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::DocumentSymbol>;

auto folding_ranges(CompilationUnitRef unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::FoldingRange>;

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

auto inlay_hints(CompilationUnitRef unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {},
                 PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::InlayHint>;

auto signature_help(CompilationParams& params, const SignatureHelpOptions& options = {})
    -> protocol::SignatureHelp;

auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range,
                     PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<protocol::TextEdit>;

}  // namespace clice::feature
