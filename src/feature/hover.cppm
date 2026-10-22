module;

#include "modules/prelude.h"

module clice:feature.hover;

import :compile.compilation;
import :feature.documentation;
import :index.peer_queries;
import :index.query;
import :index.symbol;
import :semantic.display;
import :semantic.symbol;
import :support.markup;
import :syntax.documentation;
import :syntax.position;
import :vfs.file_table;

namespace clice::feature {

namespace protocol = kota::ipc::protocol;

/// Corresponds to the `[hover]` section in clice.toml.
struct HoverOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Show what a type alias stands for, e.g. "
                         "`size_type` is `unsigned long`.")
    <bool> show_aka = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum number of members the definition of a hovered "
                         "class or enum lists: data members and member types, or "
                         "enumerators; member functions are left out. `0` shows "
                         "the declaration alone.")
    <std::uint32_t> max_members = 20;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Show how the compiler lays out a class or a field "
                         "wherever it is hovered: size, alignment, a field's "
                         "offset, and the padding.")
    <bool> layout = true;
};

/// The noun a hover card names an entity of `kind` with; empty for the
/// lexical kinds no card is about.
auto kind_noun(SymbolKind kind) -> llvm::StringRef;

/// What a hover card shows, before it is rendered: the title, the
/// declaration as code, then facts and documentation. Each field is shown
/// only when set.
struct HoverInfo {
    /// How an argument reaches the parameter it is passed to.
    enum class PassMode : std::uint8_t {
        Value,
        Ref,
        ConstRef,
        RValueRef,
    };

    /// The noun the title names the entity with ("function", "header");
    /// SymbolKind::Invalid for a card the name alone titles (`this`, an
    /// expression).
    SymbolKind kind = SymbolKind::Invalid;

    /// The name in the title, without its scope.
    std::string name;

    /// Where the entity is declared, as the title shows it: the kind of the
    /// enclosing declaration (`namespace`, `struct`, `lambda in function`)
    /// and its qualified name (`geo::Point`); both empty at global scope.
    std::string scope_kind;
    std::string scope;

    /// `private` or `protected` for a member with that access, which the
    /// title shows; empty otherwise.
    std::string access;

    /// The range of the hovered token in the focus file, which the editor
    /// highlights.
    std::optional<LocalSourceRange> symbol_range;

    /// What the documentation comment says, or what clice says of a trivial
    /// accessor or `__func__`.
    Documentation documentation;

    /// The manual's entry of an attribute, in reStructuredText.
    std::string attribute_documentation;

    /// Set for a deprecated entity: the message its deprecation gives, empty
    /// when it gives none.
    std::optional<std::string> deprecation;

    /// The declaration, shown as code.
    std::string definition;

    /// A second code block after the declaration: the definition of the
    /// class an alias names.
    std::string target_definition;

    /// What a macro use expands to, shown as code.
    std::string expansion;

    /// The file a header or a module name resolves to.
    std::string file;

    /// The type, when the declaration does not spell it: a deduced `auto`,
    /// a structured binding, a function whose return type is deduced, an
    /// expression.
    std::optional<display::Type> type;

    /// The aliases the declaration spells, each with what it stands for.
    std::vector<display::Type> aliases;

    /// The value, when it is known at compile time.
    std::optional<std::string> value;

    /// The number of characters of a string literal, its terminator left
    /// out.
    std::optional<std::uint64_t> length;

    /// For a macro a directive tests before the file defines it, the line
    /// its definition is on.
    std::optional<std::uint32_t> defined_on_line;

    /// The methods an overriding method overrides, qualified: one in each
    /// base that declares it.
    std::vector<std::string> overrides;

    /// Layout in bits: the size of a type or a field, a field's offset, its
    /// alignment, and the padding after a field or inside a type.
    std::optional<std::uint64_t> size;
    std::optional<std::uint64_t> offset;
    std::optional<std::uint64_t> padding;
    std::optional<std::uint64_t> align;

    /// Set for an argument of a call: the parameter it is passed to and how.
    std::optional<display::Param> callee_arg_info;
    std::optional<PassMode> pass_mode;

    /// Set when an implicit conversion turns the argument into the
    /// parameter's type.
    std::optional<std::string> converted_to;

    /// The macros the declaration's attributes are written with; shown
    /// last.
    std::vector<display::AttributeMacro> attribute_macros;

    markup::Document present() const;
};

/// Get the hover information for the symbol at the given offset in the
/// focus file of the unit.
auto hover_info(CompilationUnit& unit,
                std::uint32_t offset,
                const HoverOptions& options = {},
                const DocumentationOptions& documentation = {}) -> std::optional<HoverInfo>;

/// Render structured hover information as `kind` and convert its byte range
/// through the caller's current line map.
auto to_protocol_hover(const HoverInfo& info,
                       protocol::MarkupKind kind,
                       const PositionMap& map,
                       PositionEncoding encoding) -> protocol::Hover;

auto hover(CompilationUnit& unit,
           std::uint32_t offset,
           const HoverOptions& options,
           const DocumentationOptions& documentation,
           protocol::MarkupKind kind,
           PositionEncoding encoding = PositionEncoding::UTF16) -> std::optional<protocol::Hover>;

/// Assemble the read-only hover card: name, kind, where it is (`scopes`,
/// the containers outermost first) and what the index can prove from
/// stored text — no Sema products (type, value, size, aka).
auto index_hover(const index::SymbolInfo& info,
                 llvm::ArrayRef<index::SymbolInfo> scopes,
                 llvm::StringRef definition_text,
                 Documentation documentation) -> HoverInfo;

/// The card the index gives for the symbol under the cursor: name and kind
/// from the symbol tables, the definition text sliced from stored content,
/// the documentation read from the indexed text (the overload above). A
/// module's is module_hover.
auto index_hover(const index::PeerQueries& queries,
                 const index::IndexQuery::Cursor& cursor,
                 const DocumentationOptions& documentation) -> std::optional<HoverInfo>;

/// A module name's card: the module and the unit defining it, named the
/// way the user knows the file. Only the index knows the defining unit,
/// across files.
auto module_hover(const index::PeerQueries& queries,
                  const index::IndexQuery::Cursor& cursor,
                  const index::SymbolInfo& module) -> HoverInfo;

/// The card of a directive's argument, naming its target the way the user
/// knows the file; none when the link's range no longer lies in `text`.
auto directive_hover(llvm::StringRef text,
                     const PositionMap& map,
                     FileTable& files,
                     const index::DocumentLink& link,
                     protocol::MarkupKind kind) -> std::optional<protocol::Hover>;

}  // namespace clice::feature
