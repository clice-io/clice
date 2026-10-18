module;

#include "modules/prelude.h"

module clice:feature.feature;

import :compile.compilation;
import :compile.diagnostic;
import :index.query;
import :index.shard;
import :index.symbol;
import :index.tu_manifest;
import :semantic.display;
import :semantic.symbol;
import :semantic.token_class;
import :support.anomaly;
import :support.markup;
import :syntax.documentation;
import :syntax.lexical_scan;
import :syntax.position;
import :syntax.source_text;
import :vfs.ids;
import :vfs.path;

namespace clice::feature {

namespace lsp = kota::ipc::lsp;
namespace protocol = kota::ipc::protocol;

// Feature options double as their clice.toml/initializationOptions config
// sections: `defaulted = true` lets a decode leave unmentioned fields at
// their initializers, so those are the single source of every default and a
// config source only ever overlays what it names.

/// Render an absolute path as an LSP URI string.
///
/// On Windows the path is canonicalized first (lowercase drive, forward
/// slashes): LSP clients key documents by vscode-uri's lowercase-drive
/// form, and an uppercase-drive URI from the server never matches there.
inline auto to_uri(llvm::StringRef file) -> std::string {
    llvm::SmallString<256> storage;
    file = path::canonical(file, storage);
    auto uri = kota::ipc::lsp::URI::from_file_path(std::string_view(file.data(), file.size()));
    assert(uri && "a URI names an absolute path");
    return uri->str();
}

/// Corresponds to the `[code_completion]` section in clice.toml.
struct CodeCompletionOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     choices = {"placeholders", "delimiters", "none"},
                     description =
                         "What completing a call or a template inserts after the "
                         "name: \"placeholders\" its parentheses or angle brackets "
                         "with a placeholder per parameter, \"delimiters\" the empty "
                         "parentheses or brackets with the cursor between them, "
                         "\"none\" the name alone. It applies to functions, "
                         "function-like macros, variables holding a lambda and "
                         "class, alias and variable templates; an overload set "
                         "listed as one item takes the delimiters. Nothing is "
                         "added where the arguments are written already or the "
                         "name is not called, as when taking a function's "
                         "address. Clients without snippet support get the "
                         "parentheses of a call alone.")
    <std::string> arguments = "placeholders";

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Collapse an overload set into a single completion item.")
    <bool> bundle_overloads = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Complete statements such as `if` and `for` as snippets "
                         "with placeholders for their parts; otherwise only the "
                         "keyword is inserted. Ignored for clients without "
                         "snippet support.")
    <bool> statement_snippets = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "After `.` on a pointer, list the members `->` reaches and "
                         "turn the `.` into `->` when one is chosen.")
    <bool> dot_to_arrow = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Where an unqualified name is being typed, also offer "
                         "the project's names of other namespaces it matches, "
                         "inserting the qualifier they need; one in several "
                         "namespaces is offered once per namespace. Names the "
                         "file reaches already are offered as they are; for the "
                         "others see `insert_includes` and `insert_imports`. "
                         "Needs the index for names the file does not reach.")
    <bool> all_scopes = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "With `all_scopes`, also offer names declared in headers "
                         "the file does not include yet, adding the `#include` "
                         "of the header that declares them.")
    <bool> insert_includes = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "With `all_scopes`, also offer names exported by modules "
                         "the file does not import yet, adding the `import`.")
    <bool> insert_imports = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum number of completion items, the best ranked; "
                         "`0` means no limit. A list cut short is marked "
                         "incomplete, so the editor asks again as you type.")
    <std::uint32_t> limit = 0;
};

/// What the client takes from a completion item, from its completion
/// capabilities.
struct CompletionClient {
    /// Insert texts may carry placeholders.
    bool snippets = false;

    /// An item may carry both an insert and a replace range; the editor
    /// picks one by its own setting.
    bool insert_replace = false;

    /// Documentation may be markdown rather than plain text.
    bool markdown = false;
};

/// Corresponds to the `[documentation]` section in clice.toml.
struct DocumentationOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     choices = {"doxygen", "plain"},
                     description =
                         "How documentation comments are read, wherever they "
                         "show (hover, completion, signature help, `clice "
                         "query`): \"doxygen\" reads Doxygen's commands "
                         "(`@brief`, `@param`, `@return`, ...) and the "
                         "Markdown Doxygen allows, along with the GTK-Doc and "
                         "kernel-doc conventions; \"plain\" shows a comment as "
                         "written.")
    <std::string> format = "doxygen";

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "An overriding method without a documentation comment "
                         "shows the one of the method it overrides.")
    <bool> inherit = true;
};

/// The documentation of a declaration: its comment (doc_comment) read as
/// `options` say.
auto documentation(CompilationUnit& unit,
                   const clang::NamedDecl* decl,
                   const DocumentationOptions& options) -> Documentation;

/// The same of a macro.
auto documentation(CompilationUnit& unit,
                   const clang::MacroInfo* macro,
                   const DocumentationOptions& options) -> Documentation;

/// The same of an indexed symbol, from the comments in the text the index
/// read (IndexQuery::doc_comment).
auto documentation(const index::IndexQuery& query,
                   index::SymbolHash hash,
                   Fid anchor,
                   const DocumentationOptions& options) -> Documentation;

/// Write after an entry — a parameter, a return value, an exception — what
/// a documentation says of it: its direction, then its text.
void describe(markup::Paragraph& entry, llvm::StringRef direction, llvm::StringRef text);

/// Add what a card shows of a documentation first: that the entity is
/// deprecated, with the message of its attribute (`attribute`) or of its
/// comment, then the brief.
void add_summary(markup::Document& output,
                 const Documentation& documentation,
                 std::optional<llvm::StringRef> attribute);

/// Add the parameters a comment documents under `title`, each with what it
/// says of it; nothing when it documents none.
void add_parameters(markup::Document& output,
                    llvm::StringRef title,
                    llvm::ArrayRef<Documentation::Param> documented);

/// Add what a documentation says of the values a function returns or the
/// exceptions it throws, as a list under `title` (none when empty).
void add_entries(markup::Document& output,
                 llvm::StringRef title,
                 llvm::ArrayRef<Documentation::Entry> entries);

/// Add what a card shows of a documentation last: its details, then what
/// it refers to (`@see`).
void add_details(markup::Document& output, const Documentation& documentation);

/// A documentation on its own, for a reader without the declaration at
/// hand: its brief, its parameters, what it returns and throws, then its
/// details.
auto present(const Documentation& documentation) -> markup::Document;

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

/// Corresponds to the `[diagnostics]` section in clice.toml.
struct DiagnosticsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Run clang-tidy on open files and offer its fixes, with "
                         "the checks the nearest `.clang-tidy` configures (a "
                         "small default set without one). Checks too slow for "
                         "an editor, and those unreliable on code being edited, "
                         "are left out; files `clice lint` does not check get "
                         "none. `NOLINT` comments also silence compiler warnings "
                         "only while this runs.")
    <bool> clang_tidy = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Diagnostics open files do not show, by their code: a "
                         "compiler warning's option (`-Wunused-variable`), "
                         "clang's name for any other diagnostic "
                         "(`err_undeclared_var_use`), or a clang-tidy check. "
                         "For code clang gets wrong: everything else works on "
                         "the file as before, and `clice lint` still reports "
                         "them.")
    <std::vector<std::string>> suppress;
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

/// Corresponds to the `[document_symbols]` section in clice.toml.
struct DocumentSymbolsOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "List the `#define`s the file writes; an include guard's never "
                         "lists.")
    <bool> macros = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "List what a function's body declares: its variables, types and "
                         "lambdas, under the function.")
    <bool> locals = true;
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

/// Corresponds to the `[signature_help]` section in clice.toml.
struct SignatureHelpOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Show the called function's documentation comment.")
    <bool> documentation = true;
};

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

/// Result of scanning a unit for preprocessor-inactive regions.
struct InactiveScan {
    /// Byte-offset ranges [begin0, end0, begin1, end1, ...] of inactive
    /// branch bodies in the focus file; directive lines excluded.
    /// Sorted by begin and disjoint — nested inactive conditionals merge
    /// into the enclosing region.
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
    bool deprecated = false;
    std::vector<DocumentSymbol> children;

    /// The entity it declares (EntityTable), the index's symbol hash; 0 for
    /// a `#define`, a macro invocation, a mark and a module declaration.
    std::uint64_t entity = 0;
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
};

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
/// worker's AST results and the master's index projections, so both paths
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

auto folding_ranges(CompilationUnit& unit) -> std::vector<FoldingRange>;

/// The kind of the fold a declaration's block makes, by the declaration's
/// kind; none for a declaration owning no block. The AST collector and the
/// index projection share it, so a block keeps its kind when the AST
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
/// results and the master's index projections alike. A client folding
/// whole lines gets one fold per set of hidden lines; when it also shows
/// collapsed text, that text is the fold's whole first line, and a fold
/// whose closer sits on a line of its own folds that line too
/// (`int f() {...}`).
auto folding_ranges_to_protocol(llvm::ArrayRef<FoldingRange> ranges,
                                llvm::StringRef content,
                                const PositionMap& map,
                                PositionEncoding encoding,
                                const FoldingClient& client) -> std::vector<protocol::FoldingRange>;

/// The ranges an expanding selection steps through from each offset of the
/// focus file, innermost first, each strictly containing the one before:
/// the AST nodes around the offset merged with the ranges only the text
/// shows — a literal's text, a comment, the inside of a bracket pair, a
/// statement with its `;`, a directive line. An offset nothing covers gets
/// the empty range at it.
auto selection_ranges(CompilationUnit& unit, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>>;

/// The ranges only the text shows, from its lexical scan: what a document
/// without an AST gets.
auto lexical_selection_ranges(SourceText& source, llvm::ArrayRef<std::uint32_t> offsets)
    -> std::vector<std::vector<LocalSourceRange>>;

/// Protocol encoding of one offset's ranges as a chain of parents.
auto selection_range_to_protocol(llvm::ArrayRef<LocalSourceRange> ranges,
                                 const PositionMap& map,
                                 PositionEncoding encoding) -> protocol::SelectionRange;

/// One declaration, macro or module declaration the outline lists, before
/// it is named from the text and nested: the AST and the index each give
/// these, and outline() makes both into the same tree.
struct OutlineEntry {
    /// The name the declaration goes by (display::symbol_name); outline()
    /// adds the qualifier and template arguments the text writes.
    std::string name;
    std::string detail;
    SymbolKind kind = SymbolKind::Invalid;
    LocalSourceRange range;
    LocalSourceRange selection_range;
    bool deprecated = false;

    /// The entity declared and the one it is named in (EntityTable), 0
    /// for macros and modules.
    std::uint64_t entity = 0;
    std::uint64_t parent = 0;

    bool definition = false;

    /// The name is a presentation, `(anonymous struct)`.
    bool unnamed = false;

    /// The name comes out of a macro's body or a token paste and is
    /// written nowhere: the selection range is the macro invocation's.
    bool from_macro = false;
};

/// Whether the outline lists a declaration of `kind`: a nameless one only
/// when it is a scope, a template parameter never.
bool outline_lists(SymbolKind kind, bool unnamed, bool template_parameter);

/// The outline the entries make in the file `source` holds, with its
/// `#define` directives (`macros`, those the preprocessor read), module
/// declarations and `#pragma mark`s (`marks`): the entries are named as the
/// text writes them, members of one macro invocation listed under it,
/// nested by where they are written, and sections made of the marks.
auto outline(std::vector<OutlineEntry> entries,
             SourceText& source,
             llvm::ArrayRef<LexicalInfo::Directive> macros,
             llvm::ArrayRef<LexicalInfo::ModuleDeclaration> modules,
             llvm::ArrayRef<LexicalInfo::Directive> marks,
             const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// The outline of the focus file, a `#define` in it only outside the
/// inactive regions. The overload without a regions argument scans the
/// unit itself — correct only for units compiled without a PCH, as with
/// semantic_tokens.
auto document_symbols(CompilationUnit& unit, const DocumentSymbolsOptions& options)
    -> std::vector<DocumentSymbol>;
auto document_symbols(CompilationUnit& unit,
                      llvm::ArrayRef<std::uint32_t> inactive_regions,
                      const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// How a client takes document symbols: as a tree when it declares
/// hierarchicalDocumentSymbolSupport, flat (`SymbolInformation`)
/// otherwise; in every kind when it declares a set of them (and so takes
/// others gracefully), in the protocol's first 18 otherwise.
struct SymbolClient {
    bool hierarchical = false;
    bool all_kinds = false;
};

auto document_symbols_to_protocol(llvm::ArrayRef<DocumentSymbol> symbols,
                                  const PositionMap& map,
                                  PositionEncoding encoding,
                                  const SymbolClient& client)
    -> std::vector<protocol::DocumentSymbol>;

/// The symbols flat, for a client taking no tree: each in `uri`, naming the
/// symbol it nests in.
auto document_symbols_to_information(llvm::ArrayRef<DocumentSymbol> symbols,
                                     const PositionMap& map,
                                     PositionEncoding encoding,
                                     const SymbolClient& client,
                                     llvm::StringRef uri)
    -> std::vector<protocol::SymbolInformation>;

auto inlay_hints(CompilationUnit& unit,
                 LocalSourceRange target,
                 const InlayHintsOptions& options = {}) -> std::vector<InlayHint>;

/// Protocol encoding of computed hints against the text they describe. A label
/// with a piece naming a symbol or a target keeps its pieces when `locate`
/// gives the location each such piece navigates to; without it, and for
/// every other label, the label is one string.
auto inlay_hints_to_protocol(
    llvm::ArrayRef<InlayHint> hints,
    const PositionMap& map,
    PositionEncoding encoding,
    llvm::function_ref<std::optional<protocol::Location>(const InlayHintPart&)> locate = nullptr)
    -> std::vector<protocol::InlayHint>;

/// Include-directive links of the focus file, in byte offsets; the
/// reply edge converts them with the session's line map.
auto document_links(CompilationUnit& unit) -> std::vector<index::DocumentLink>;

/// Scan the focus file's condition directives. `open_stack` seeds the
/// nesting state (from a preceding preamble scan) and `resume_offset` is
/// where the scanned content starts — pending inactive levels from the
/// seed begin there. A scan that ends with open levels closes their
/// pending regions at `end_offset` (the content bound).
InactiveScan inactive_regions(CompilationUnit& unit,
                              llvm::ArrayRef<std::uint8_t> open_stack = {},
                              std::uint32_t resume_offset = 0,
                              std::uint32_t end_offset = UINT32_MAX);

/// Where a diagnostic shown on a file was raised, when not in the file's
/// own text.
enum class DiagnosticOrigin : std::uint8_t {
    /// The file's text, or the command line.
    File,

    /// A template the file instantiates: shown where the file asks for the
    /// instantiation.
    Template,

    /// A header the file includes: shown on its #include.
    IncludedFile,

    /// The includer's code a header's borrowed context compiles around it.
    IncluderContext,
};

/// A place a diagnostic's note points at.
struct DiagnosticNote {
    std::string path;
    protocol::Range range;
    std::string message;
};

/// A diagnostic as shown on one file, in the file's positions: the
/// compiler's and clang-tidy's of its compile, and clice's own about the
/// file. Each client gets it in the shape it takes (to_protocol).
struct FileDiagnostic {
    protocol::Range range;
    DiagnosticLevel level = DiagnosticLevel::Warning;
    DiagnosticSource source = DiagnosticSource::Clice;

    /// clang's ID of a compiler diagnostic; 0 for the others.
    std::uint32_t id = 0;

    /// See DiagnosticID::diagnostic_code.
    std::string code;

    /// The page documenting the code; empty for none.
    std::string link;

    /// The message as reported, without where it came from.
    std::string message;

    DiagnosticOrigin origin = DiagnosticOrigin::File;

    std::vector<DiagnosticNote> notes;
};

/// The diagnostics a compile shows on its focus file.
auto diagnostics(CompilationUnit& unit, PositionEncoding encoding = PositionEncoding::UTF16)
    -> std::vector<FileDiagnostic>;

/// A warning from clice itself about the file as a whole, on its first
/// character.
FileDiagnostic file_warning(std::string message);

/// The LSP form of `diagnostic`. A client that takes no related
/// information gets the notes in the message.
protocol::Diagnostic to_protocol(const FileDiagnostic& diagnostic, bool related_information);

/// A name of another namespace unqualified completion may offer, found in
/// the index: the completion looks it up to tell whether the file reaches
/// it already (CodeCompletionOptions::all_scopes).
struct ScopeCandidate {
    std::string name;

    /// The namespaces around it, outermost first, inline ones left out;
    /// an enumerator's enumeration last.
    std::vector<std::string> scope;

    protocol::CompletionItemKind kind = protocol::CompletionItemKind::Text;
    bool is_template = false;
    bool deprecated = false;

    /// The module exporting it, `name` or `name:partition`, when a module
    /// unit declares it.
    std::string module;

    /// The symbol, by which the server picks the header declaring it.
    std::uint64_t symbol = 0;
};

/// Where an `#include` a completion item needs goes.
struct IncludePlace {
    protocol::Position position;

    /// A module unit without a global module fragment opens one there.
    bool opens_fragment = false;

    /// The file is C: a standard name's header is the C library's.
    bool c = false;
};

/// A completion's reply before the server finishes it: an item whose
/// `data` holds a symbol (ScopeCandidate::symbol) names what the file does
/// not include yet, and is whole once it adds the `#include` of the header
/// declaring it, at `include`. With such items the list is not cut to
/// `code_completion.limit` yet: one whose header is unknown goes first.
struct Completion {
    protocol::CompletionList list;
    std::optional<IncludePlace> include;
};

auto code_complete(CompilationParams params,
                   const CodeCompletionOptions& options = {},
                   const DocumentationOptions& documentation = {},
                   const CompletionClient& client = {},
                   PositionEncoding encoding = PositionEncoding::UTF16,
                   llvm::ArrayRef<ScopeCandidate> candidates = {}) -> Completion;

/// The identifier typed at `offset` of `text`, from its start to `offset`,
/// when nothing qualifies it — no `::`, `.` or `->` before it — and it is
/// not empty: where a completion may offer names of other namespaces.
auto unqualified_prefix(llvm::StringRef text, std::uint32_t offset)
    -> std::optional<llvm::StringRef>;

/// The reply listing `items` under `code_completion.limit`: the best ranked
/// stay, ties by label, and a list cut short is marked incomplete.
auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList;

/// Get the hover information for the symbol at the given offset in the
/// focus file of the unit.
auto hover_info(CompilationUnit& unit,
                std::uint32_t offset,
                const HoverOptions& options = {},
                const DocumentationOptions& documentation = {}) -> std::optional<HoverInfo>;

/// The card of a declaration of any file of the unit, as hovering its name
/// shows it.
auto hover_info(CompilationUnit& unit, const clang::NamedDecl& decl, const HoverOptions& options)
    -> HoverInfo;

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

/// `selected` is the label of the signature the client shows active: it
/// stays active while it is listed.
auto signature_help(CompilationParams params,
                    const SignatureHelpOptions& options,
                    const DocumentationOptions& documentation,
                    llvm::StringRef selected = {}) -> protocol::SignatureHelp;

/// The edits formatting `content`, or its `range`, with the clang-format
/// style of `file`: the nearest `.clang-format` above it, else LLVM's. Why
/// that style cannot be read otherwise.
auto document_format(llvm::StringRef file,
                     llvm::StringRef content,
                     std::optional<LocalSourceRange> range = std::nullopt)
    -> std::expected<std::vector<TextReplacement>, std::string>;

/// One kind per refactoring, below the standard `refactor.rewrite` and
/// `refactor.inline`, so that a client can ask for one alone: an editor
/// binds a key to it. Fixes are all `quickfix`.
namespace action_kind {

constexpr inline std::string_view define_inline = "refactor.rewrite.define.inline";
constexpr inline std::string_view define_out_of_line = "refactor.rewrite.define.outOfLine";
constexpr inline std::string_view define_missing = "refactor.rewrite.define.missing";
constexpr inline std::string_view implement_pure_virtuals =
    "refactor.rewrite.implementPureVirtuals";
constexpr inline std::string_view memberwise_constructor =
    "refactor.rewrite.generateMemberwiseConstructor";
constexpr inline std::string_view populate_switch = "refactor.rewrite.populateSwitch";
constexpr inline std::string_view expand_deduced_type = "refactor.rewrite.expandDeducedType";
constexpr inline std::string_view reorder_definitions = "refactor.rewrite.reorderDefinitions";
constexpr inline std::string_view expand_macro = "refactor.inline.macro";

}  // namespace action_kind

/// The kinds the actions produce: the advertised capability, and what a
/// request's `only` filter is matched against.
constexpr inline std::array code_action_kinds = {
    protocol::CodeActionKind::QuickFix,
    action_kind::define_inline,
    action_kind::define_out_of_line,
    action_kind::define_missing,
    action_kind::implement_pure_virtuals,
    action_kind::memberwise_constructor,
    action_kind::populate_switch,
    action_kind::expand_deduced_type,
    action_kind::reorder_definitions,
    action_kind::expand_macro,
};

/// One definition the index vets: dropped when any source knows a
/// definition of its symbol — this TU proved nothing defines it here,
/// another TU may.
struct DefinitionPiece {
    std::uint64_t entity = 0;
    std::string text;
};

/// Definitions to insert into the focus file once the index vetted them:
/// the surviving pieces joined by blank lines replace `range`, wrapped in
/// `before` and `after`, formatted by whoever assembles them.
struct DefineRequest {
    LocalSourceRange range;
    std::string before;
    std::string after;
    std::vector<DefinitionPiece> pieces;
};

/// Definitions to insert into the file's host source once the index
/// vetted them: after the definitions of `container`'s members there, at
/// its end when it has none (a container of 0 goes to the end outright).
/// The texts are fully qualified, valid at namespace scope anywhere in
/// the host.
struct DefineInHostRequest {
    std::uint64_t container = 0;
    std::vector<DefinitionPiece> pieces;
};

/// Where a new `#include` line goes in the focus file. A module unit
/// without a global module fragment gets one opened there.
struct IncludeInsertion {
    std::uint32_t offset = 0;
    bool opens_fragment = false;

    /// The text inserting `#include <header>`, the header spelled with its
    /// quotes or brackets.
    std::string text(llvm::StringRef header) const {
        return std::format("{}#include {}\n", opens_fragment ? "module;\n" : "", header);
    }
};

/// One action per header declaring `name` under `scope` ("std::" style,
/// empty for an unqualified name), each inserting its include directive
/// at `insertion` of the focus file.
struct IncludeRequest {
    std::string scope;
    std::string name;
    IncludeInsertion insertion;
};

/// What only the project index can decide: resolved by the master at the
/// reply edge; clice inspect resolves it as an empty index would.
using IndexRequest = std::variant<DefineRequest, DefineInHostRequest, IncludeRequest>;

struct CodeAction {
    std::string title;
    protocol::CodeActionKind kind;
    std::vector<TextReplacement> edits;
    std::optional<IndexRequest> index;
};

/// The code actions available on a selection of the focus file. Every
/// action is computed to completion against this AST: single-TU actions
/// carry their edits, the ones needing the index carry their request.
auto code_actions(CompilationUnit& unit, LocalSourceRange selection) -> std::vector<CodeAction>;

/// The pieces the index keeps, joined by blank lines; nullopt when none
/// survives.
auto assemble_definitions(llvm::ArrayRef<DefinitionPiece> pieces,
                          llvm::function_ref<bool(std::uint64_t entity)> defined_elsewhere)
    -> std::optional<std::string>;

/// Reformat the lines `edits` touch with the clang-format style configured
/// for the file, folding the formatting back into replacements of the
/// original text. Without a configured style, or with one that disables
/// formatting, the edits come back unchanged.
auto format_edits(llvm::StringRef file, llvm::StringRef content, std::vector<TextReplacement> edits)
    -> std::vector<TextReplacement>;

/// `edits` cleaned up as clang-tidy cleans up the fixes it applies: on the
/// lines they touch, a comma left after `(`, before `)` or doubled, a
/// constructor's `:` with no initializer left, a namespace left empty.
auto cleanup_edits(llvm::StringRef file,
                   llvm::StringRef content,
                   std::vector<TextReplacement> edits) -> std::vector<TextReplacement>;

/// `text` reformatted as a standalone snippet with the style configured
/// for `file`; unchanged without one.
auto format_snippet(llvm::StringRef file, llvm::StringRef text) -> std::string;

/// Index projections: whole-document features computed from index entries plus
/// the document text, serving open files that have no AST yet (see
/// Features' routing rules). Inputs are index vocabulary types and
/// narrow resolvers, never index storage — the caller extracts entries from
/// shard/ProjectIndex and hands them over. Each projection's output is an
/// honest subset of its AST twin's: missing pieces (Sema modifiers, outline
/// detail, statement folds, ...) are pinned by the index/read_only cases.

/// One Decl/Def relation of the document: the name-token anchor, the
/// declaration's full extent (invalid when the index recorded none — a
/// cross-file or invalid source range) and the symbol it belongs to.
struct IndexDecl {
    LocalSourceRange range;
    LocalSourceRange extent;
    index::SymbolHash symbol = 0;
    bool definition = false;
};

/// The entries of one document the projections consume, extracted from its
/// serving shard: every occurrence, the reference relations of names nested in
/// another's written name (the class in `~Foo`, which owns no occurrence
/// there) as occurrences of their own, and the Decl/Def relations as decl
/// entries.
struct IndexEntries {
    std::vector<index::Occurrence> occurrences;
    std::vector<IndexDecl> decls;

    /// The explicit instantiations, as decl entries of what they
    /// instantiate (RelationKind::Instantiation).
    std::vector<IndexDecl> instantiations;

    /// The names written to (RelationKind::Write).
    std::vector<index::Occurrence> writes;
};

auto extract_index_entries(const index::Shard& shard) -> IndexEntries;

using IndexIncludeEdge = index::IncludeEdge;

using IndexSymbolResolver = llvm::function_ref<std::optional<index::SymbolInfo>(index::SymbolHash)>;

/// The tokens of `source` as the AST's would be, from what the index
/// records: each symbol's classified from `resolve` at its occurrences, the
/// roles `decls` give, the writes `writes` record.
auto index_semantic_tokens(SourceText& source,
                           const IndexEntries& entries,
                           IndexSymbolResolver resolve,
                           const SemanticTokensOptions& options) -> std::vector<SemanticToken>;

/// The outline's SymbolKind -> LSP mapping, exhaustive over the enum so a
/// new kind fails to compile rather than fall into a default. The index
/// navigation surfaces layer their own policy for the kinds whose display
/// differs (see to_lsp::symbol_kind).
auto to_protocol_symbol_kind(SymbolKind kind) -> protocol::SymbolKind;

/// The outline of the file `source` holds, from its index entries: the
/// AST's (outline()) without details, a macro's parameters aside.
auto index_document_symbols(SourceText& source,
                            const IndexEntries& entries,
                            IndexSymbolResolver resolve,
                            const DocumentSymbolsOptions& options) -> std::vector<DocumentSymbol>;

/// Declaration folds: each definition extent folds its last balanced brace
/// group (brace matching over a raw lex, so braces in strings, comments
/// and directive lines do not count), keeping the name and signature
/// visible like the AST folds do. Extents touching a conditional whose
/// branches unbalance braces produce no fold — a raw pairing there is
/// wrong for some preprocessing variant.
auto index_folding_ranges(SourceText& source,
                          llvm::ArrayRef<IndexDecl> decls,
                          IndexSymbolResolver resolve) -> std::vector<FoldingRange>;

/// Document links from manifest include edges: each edge's argument span is
/// located with find_directive_argument on its directive line. Lines the
/// manifest has no edge for (guard-skipped includes, __has_include, #embed)
/// produce no link.
auto index_document_links(SourceText& source, llvm::ArrayRef<IndexIncludeEdge> edges)
    -> std::vector<index::DocumentLink>;

/// Assemble the read-only hover card: name, kind, where it is (`scopes`,
/// the containers outermost first) and what the index can prove from
/// stored text — no Sema products (type, value, size, aka).
auto index_hover(const index::SymbolInfo& info,
                 llvm::ArrayRef<index::SymbolInfo> scopes,
                 llvm::StringRef definition_text,
                 Documentation documentation) -> HoverInfo;

}  // namespace clice::feature
