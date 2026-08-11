#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "compile/compilation_unit.h"
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
                     description = "Insert function arguments as a snippet on completion.")
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
                     description = "Render the hover card as markdown rather than plain text.")
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
    KOTATSU_ANNOTATE(defaulted = true, description = "Master switch for inlay hints.")
    <bool> enabled = true;

    KOTATSU_ANNOTATE(defaulted = true, description = "Show parameter name hints at call sites.")
    <bool> parameters = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Show deduced types for `auto` and templated declarations.")
    <bool> deduced_types = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Show designators in aggregate initialization.")
    <bool> designators = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Show a hint naming the construct after a closing brace.")
    <bool> block_end = false;

    KOTATSU_ANNOTATE(defaulted = true, description = "Show omitted default arguments.")
    <bool> default_arguments = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Character budget for a rendered type name; longer names "
                         "are truncated; 0 means no limit.")
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
/// the PCH's PreambleState blob as-is and becomes an LSP DocumentLink only
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

/// Include-directive links of the interested file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnitRef unit) -> std::vector<DocumentLink>;

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
