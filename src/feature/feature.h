#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compile/compilation.h"
#include "compile/compilation_unit.h"
#include "feature/document_link.h"
#include "semantic/symbol_kind.h"
#include "support/anomaly.h"
#include "support/markup.h"

#include "kota/ipc/lsp/position.h"
#include "kota/ipc/lsp/protocol.h"
#include "kota/ipc/lsp/uri.h"

namespace clice::feature {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

using kota::ipc::lsp::LineMap;
using kota::ipc::lsp::PositionEncoding;

/// Render a file path (or an already-formed URI) as an LSP URI string.
inline auto to_uri(llvm::StringRef file) -> std::string {
    const auto file_view = std::string_view(file.data(), file.size());

    if(auto parsed = kota::ipc::lsp::URI::parse(file_view)) {
        return parsed->str();
    }

    if(auto uri = kota::ipc::lsp::URI::from_file_path(file_view)) {
        return uri->str();
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

struct CodeCompletionOptions {
    bool enable_keyword_snippet = false;
    bool enable_function_arguments_snippet = false;
    bool enable_template_arguments_snippet = false;
    bool insert_paren_in_function_call = false;
    bool bundle_overloads = true;
    std::uint32_t limit = 0;
};

struct HoverOptions {
    /// Render the hover card as markdown rather than plain text.
    bool parse_comment_as_markdown = true;

    /// Show the desugared form of a type, e.g. `vector<int>::size_type (aka
    /// unsigned long)`.
    bool show_aka = true;
};

/// Contains detailed information about a symbol. Especially useful when
/// generating hover responses. It can be rendered as a hover panel, or
/// embedding clients can use the structured information to provide their own
/// UI.
struct HoverInfo {
    /// Contains pretty-printed type and desugared type.
    struct PrintedType {
        PrintedType() = default;

        PrintedType(llvm::StringRef type) : type(type.str()) {}

        PrintedType(llvm::StringRef type, llvm::StringRef aka) : type(type.str()), aka(aka.str()) {}

        /// Allow assigning string literals to std::optional<PrintedType>,
        /// which would otherwise require two implicit user conversions.
        PrintedType(const char* type) : PrintedType(llvm::StringRef(type)) {}

        PrintedType(const char* type, const char* aka) :
            PrintedType(llvm::StringRef(type), llvm::StringRef(aka)) {}

        bool operator==(const PrintedType&) const = default;

        /// Pretty-printed type.
        std::string type;

        /// Desugared type.
        std::optional<std::string> aka;
    };

    /// Represents parameters of a function or a template.
    /// For example:
    /// - void foo(ParamType Name = DefaultValue)
    /// - template <ParamType Name = DefaultType> class Foo {};
    struct Param {
        bool operator==(const Param&) const = default;

        /// The printable parameter type, e.g. "int", or "typename" (in
        /// template parameters).
        std::optional<PrintedType> type;

        /// std::nullopt for unnamed parameters.
        std::optional<std::string> name;

        /// std::nullopt if no default is provided.
        std::optional<std::string> default_value;
    };

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
    std::optional<PrintedType> type;

    /// Set for functions and lambdas.
    std::optional<PrintedType> return_type;

    /// Set for functions and lambdas with parameters.
    std::optional<std::vector<Param>> parameters;

    /// Set for all templates (function, class, variable).
    std::optional<std::vector<Param>> template_parameters;

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
    std::optional<Param> callee_arg_info;

    /// Set only if callee_arg_info is set.
    std::optional<PassType> call_pass_type;

    /// Produce a user-readable information.
    markup::Document present() const;
};

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const HoverInfo::PrintedType& type);

llvm::raw_ostream& operator<<(llvm::raw_ostream& os, const HoverInfo::Param& param);

/// Try to infer structure of a documentation comment (e.g. line breaks).
void parse_documentation(llvm::StringRef input, markup::Document& output);

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

/// Include-directive links of the interested file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnitRef unit) -> std::vector<DocumentLink>;

/// Go-to-definition on an include directive: when `offset` falls on the
/// argument of an #include or __has_include in the interested file, the
/// resolved file's location (at its start). Empty otherwise.
auto include_definition(CompilationUnitRef unit, std::uint32_t offset)
    -> std::vector<protocol::Location>;

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
