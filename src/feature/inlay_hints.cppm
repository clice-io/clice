module;

#include "modules/prelude.h"

module clice:feature.inlay_hints;

import :compile.compilation;
import :index.peer_queries;
import :index.symbol;
import :syntax.position;
import :vfs.ids;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// Corresponds to the `[inlay_hints.max_length]` section in clice.toml.
struct InlayHintLengths {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum length in characters of a deduced type or of "
                         "deduced template arguments; `0` means no limit. The "
                         "template arguments that do not fit become `…`, e.g. "
                         "`tuple<int, string, …>`; a type whose outermost name "
                         "does not fit is not shown.")
    <std::uint32_t> types = 32;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum length in characters of the omitted default "
                         "arguments; `0` means no limit. One too long becomes "
                         "`…`, and the list is cut with `…` where it no longer "
                         "fits.")
    <std::uint32_t> default_arguments = 32;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum length in characters of a block end or "
                         "namespace end label, cut with `…` past it; `0` means "
                         "no limit.")
    <std::uint32_t> block_end = 60;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum length in characters of an `#endif` condition, "
                         "cut with `…` past it; `0` means no limit.")
    <std::uint32_t> endif = 60;
};

/// Corresponds to the `[inlay_hints]` section in clice.toml. Every switch
/// shows one kind of hint.
struct InlayHintsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Master switch: `false` disables all inlay hints.")
    <bool> enabled = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Parameter name hints at call sites, e.g. `draw(width: "
                         "800, height: 600)`.")
    <bool> parameters = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "`&` marking an argument passed by mutable reference, "
                         "e.g. `mutate(&value: v)`.")
    <bool> mutable_references = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Parameter name hints for an argument named like the "
                         "parameter without its one-letter prefix, e.g. `param` "
                         "passed to `aParam`.")
    <bool> prefixed_names = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Parameter name hints for a function taking one "
                         "parameter whose name is a single letter, e.g. "
                         "`push_back(x: 1)`.")
    <bool> single_letter_names = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Deduced type hints for `auto` variables, structured "
                         "bindings, init-captures and `auto` parameters, the type "
                         "a `decltype` names, and deduced return types.")
    <bool> deduced_types = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Deduced type hints for a variable whose initializer "
                         "already names its type, e.g. `static_cast<long>(x)`, "
                         "`Widget{1, 2}` or `std::make_unique<Widget>()`.")
    <bool> spelled_types = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The template arguments class template argument "
                         "deduction found, e.g. `<int, double>` after `pair` in "
                         "`std::pair pair(1, 2.5)`.")
    <bool> deduced_template_arguments = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Field designator hints in aggregate initialization, "
                         "e.g. `.x=` and `.y=` in `Point{1, 2}`.")
    <bool> designators = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Array index designator hints in aggregate "
                         "initialization, e.g. `[0]=` and `[1]=` in "
                         "`int pair[2] = {1, 2}`.")
    <bool> array_indices = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "`// name` after the closing brace of a function, type "
                         "or control flow block, linking to its start.")
    <bool> block_end = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "`// namespace name` after the closing brace of a "
                         "namespace, linking to its start.")
    <bool> namespace_end = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "`// condition` after an `#endif`, the condition of the "
                         "`#if` it closes, linking to it.")
    <bool> endif = true;

    KOTATSU_ANNOTATE(defaulted = true, description = "The default arguments a call omitted.")
    <bool> default_arguments = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The height in lines, both ends included, from which a "
                         "block, namespace or conditional gets a hint at its end.")
    <std::uint32_t> min_block_lines = 10;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The [inlay_hints.max_length] table: how long each kind "
                         "of hint may grow.")
    <InlayHintLengths> max_length;
};

enum class HintCategory : std::uint8_t {
    Parameter,
    DefaultArgument,
    Type,
    Designator,
    BlockEnd,
};

/// A piece of an inlay hint's label; the pieces in order are the label.
struct InlayHintPart {
    std::string value;

    /// The symbol the piece names, which navigation and hover on the piece
    /// reach; 0 for plain text.
    index::SymbolHash symbol = 0;

    /// A file holding the symbol's entries, for a symbol only its own file or
    /// translation unit names (a parameter, an internal type); empty for one
    /// every unit names.
    std::string anchor;

    /// The focus-file offset a piece naming no symbol navigates to: where the
    /// block a block end hint closes starts.
    std::optional<std::uint32_t> target;

    friend auto operator<=>(const InlayHintPart&, const InlayHintPart&) = default;
};

struct InlayHint {
    std::uint32_t offset = 0;
    HintCategory kind = HintCategory::Type;
    std::vector<InlayHintPart> label;
    bool padding_left = false;
    bool padding_right = false;

    friend bool operator==(const InlayHint&, const InlayHint&) = default;
};

auto inlay_hints(CompilationUnit& unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {}) -> std::vector<InlayHint>;

/// Protocol encoding of computed hints against the text they describe. A label
/// with a piece naming a symbol or a target keeps its pieces when `locate`
/// gives the location each such piece navigates to; without it, and for
/// every other label, the label is one string.
auto to_protocol(
    llvm::ArrayRef<InlayHint> hints,
    const PositionMap& map,
    PositionEncoding encoding,
    llvm::function_ref<std::optional<protocol::Location>(const InlayHintPart&)> locate = nullptr)
    -> std::vector<protocol::InlayHint>;

/// The same with every piece naming a symbol or a target located: a
/// target at that offset of `file`; a symbol, over the queries, at a
/// declaration when one exists — the client runs go-to-definition there,
/// and the definition answers it — else at its definition, which answers
/// itself.
auto to_protocol(llvm::ArrayRef<InlayHint> hints,
                 const PositionMap& map,
                 PositionEncoding encoding,
                 const index::PeerQueries& queries,
                 Fid file) -> std::vector<protocol::InlayHint>;

}  // namespace clice::feature
