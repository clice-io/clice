module;

#include "modules/prelude.h"

module clice:feature.semantic_tokens;

import :compile.compilation;
import :feature.index_entries;
import :index.query;
import :index.tu_index;
import :semantic.token_class;
import :syntax.position;
import :syntax.source_text;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// Corresponds to the `[semantic_tokens]` section in clice.toml.
struct SemanticTokensOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Send the tokens an editor's own syntax highlighting colors as well: "
                         "keywords, literals, comments, operators, preprocessor directives. "
                         "When off, only what syntax cannot tell is sent: names by what they "
                         "name, overloaded and declarator operators, template brackets, format "
                         "specifiers, and the tokens of preprocessor-inactive code, kindless.")
    <bool> lexical = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The names a client or theme expects for clice's token kinds, as "
                         "`field = \"property\"`; an empty name drops the kind, whose tokens "
                         "are then not sent, and kinds given one name merge. Read when the "
                         "server starts.")
    <std::map<std::string, std::string>> kinds;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The names a client or theme expects for clice's token modifiers; an "
                         "empty name drops the modifier, and modifiers given one name merge. "
                         "Read when the server starts.")
    <std::map<std::string, std::string>> modifiers;
};

struct SemanticToken {
    LocalSourceRange range;
    SemanticTokenKind kind = SemanticTokenKind::None;
    std::uint32_t modifiers = 0;
};

/// The legend clice's tokens are sent with: the names of their kinds and
/// modifiers after the `[semantic_tokens]` renames, and where each goes.
struct SemanticTokenLegend {
    constexpr static auto kind_count = kota::meta::reflection<SemanticTokenKind>::member_count;
    constexpr static auto modifier_count =
        kota::meta::reflection<SemanticTokenModifier>::member_count;

    std::vector<std::string> types;
    std::vector<std::string> modifiers;

    /// The index in `types` of each SemanticTokenKind, none for one dropped.
    std::array<std::optional<std::uint32_t>, kind_count> type_of{};

    /// The legend bit of each SemanticTokenModifier, 0 for one dropped.
    std::array<std::uint32_t, modifier_count> bit_of{};

    /// The legend bits of a token's modifier set.
    std::uint32_t bits(std::uint32_t modifiers) const {
        std::uint32_t result = 0;
        for(std::size_t modifier = 0; modifier < modifier_count; modifier += 1) {
            if(modifiers & (std::uint32_t(1) << modifier)) {
                result |= bit_of[modifier];
            }
        }
        return result;
    }

    /// The names the options map that name no kind or modifier.
    std::vector<std::string> unknown;
};

/// The name the legend gives a kind or a modifier without a rename: its
/// enumerator's, lower camel case.
std::string legend_name(SemanticTokenKind kind);
std::string legend_name(SemanticTokenModifier modifier);

auto semantic_token_legend(const SemanticTokensOptions& options) -> SemanticTokenLegend;

/// A semantic token as a client reads it: its kind and modifiers by the
/// names the legend gives them.
struct NamedToken {
    LocalSourceRange range;
    std::string kind;
    std::vector<std::string> modifiers;
};

/// The tokens the legend names, as a client reads them: one of a kind the
/// legend drops is none, a modifier it drops is not there.
auto named_tokens(llvm::ArrayRef<SemanticToken> tokens, const SemanticTokenLegend& legend)
    -> std::vector<NamedToken>;

/// Semantic tokens of the focus file; `inactive_regions` are the unit's,
/// the preamble's under a PCH included, as the worker assembles them at
/// compile time. The overload without them scans the unit itself — correct
/// only for a unit compiled without a PCH.
auto semantic_tokens(CompilationUnit& unit,
                     llvm::ArrayRef<std::uint32_t> inactive_regions,
                     const SemanticTokensOptions& options) -> std::vector<SemanticToken>;
auto semantic_tokens(CompilationUnit& unit, const SemanticTokensOptions& options)
    -> std::vector<SemanticToken>;

/// Protocol encoding of computed tokens against the text they describe,
/// those overlapping `window` alone when one is given — one encoder for the
/// worker's AST results and the master's results from the index, so both paths
/// emit byte-identical replies.
auto semantic_tokens_to_protocol(llvm::ArrayRef<SemanticToken> tokens,
                                 const PositionMap& map,
                                 PositionEncoding encoding,
                                 const SemanticTokenLegend& legend,
                                 std::optional<LocalSourceRange> window = std::nullopt)
    -> protocol::SemanticTokens;

/// The one edit turning the encoded tokens `before` into `after`: what
/// lies between their common prefix and suffix; none when they are equal.
auto semantic_tokens_edits(llvm::ArrayRef<std::uint32_t> before,
                           llvm::ArrayRef<std::uint32_t> after)
    -> std::vector<protocol::SemanticTokensEdit>;

/// The tokens of `source` as the AST's would be, from what the index
/// records: each symbol's classified from `resolve` at its occurrences, the
/// roles `decls` give, the writes `writes` record.
auto index_semantic_tokens(SourceText& source,
                           const IndexEntries& entries,
                           IndexSymbolResolver resolve,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken>;

/// The same from the index serving the file, `source`'s text the one it
/// indexed or a buffer byte-identical to it.
auto index_semantic_tokens(SourceText& source,
                           const index::IndexQuery& query,
                           const index::IndexSource& index,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken>;

/// Adds to the macros the preamble region names the modifiers their
/// definitions give, which only the PCH's index (`preamble`) knows: neither
/// the worker's AST nor the per-edit index covers the directives the
/// preamble ran.
void merge_preamble_macros(const index::TUIndex& preamble, std::vector<SemanticToken>& tokens);

}  // namespace clice::feature
