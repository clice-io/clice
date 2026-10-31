module clice:feature.code_completion;

import :command.search_config;
import :compile.compilation;
import :feature.documentation;
import :index.query;
import :index.search;
import :support.json;
import :support.prelude;
import :syntax.completion;
import :syntax.dependency_graph;
import :syntax.position;
import :vfs.dir_cache;
import :vfs.file_table;
import :vfs.ids;

namespace clice::feature {

/// What completing a call or a template inserts after the name
/// (CodeCompletionOptions::arguments).
enum class CompletionArguments : std::uint8_t {
    Placeholders,
    Delimiters,
    None,
};

/// Corresponds to the `[code_completion]` section in clice.toml.
struct CodeCompletionOptions {
    KOTATSU_ANNOTATE(defaulted = true,
                     enum_string = type<LowercaseName>,
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
    <CompletionArguments> arguments = CompletionArguments::Placeholders;

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
                         "With `all_scopes`, also offer names declared, and macros "
                         "defined, in headers the file does not include yet, adding "
                         "the `#include` of the header that holds them.")
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
    <std::uint32_t> limit = 100;
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

/// A name completion may offer from the index, its preamble's or the
/// project's: the completion looks it up to tell whether the file reaches
/// it already, and offers one it does not reach with the qualifier, the
/// include or the import that does (CodeCompletionOptions::all_scopes).
struct ScopeCandidate {
    std::string name;

    /// The namespaces around it, outermost first, inline ones left out;
    /// an enumerator's enumeration last.
    std::vector<std::string> scope;

    protocol::CompletionItemKind kind = protocol::CompletionItemKind::Text;

    /// A macro, named in no namespace and declaring nothing.
    bool macro = false;

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

/// What a compile of the code completes with (code_complete): the names
/// the index offers, and whether it holds more of them.
struct CompileCompletion {
    std::vector<ScopeCandidate> candidates;
    bool more_candidates = false;

    /// The candidates stand in for the namespaces' names the precompiled
    /// preamble declares: the compile reads no others of them (clang's
    /// LoadExternal off), none after `.`. Not where the index names
    /// nothing, as in a directive, nor where no index of the document's PCH
    /// is at hand: the compile reads them all then.
    bool from_index = false;

    /// How much of the buffer the PCH whose index named the candidates
    /// stands for: a compile behind another PCH, as after an include added
    /// since, reads all of its names. None after `.`.
    std::optional<std::uint32_t> preamble;

    /// Where the name being typed is looked up, with from_index.
    index::CompletionScope scope;
};

/// A type, or a template argument, of one compile spelled by the names
/// that find it again in another compile of the same code: parts in
/// pre-order, each followed by those it is made of.
struct TypePart {
    enum class Kind : std::uint8_t {
        /// `value` is the clang::BuiltinType::Kind.
        Builtin,
        Pointer,
        LValueReference,
        RValueReference,
        /// Of `value` elements.
        Array,
        /// The return type, then the parameters.
        Function,
        /// A class or an enumeration, by `declarations`.
        Tag,
        /// The class of the class template `declarations` names.
        Pattern,
        /// A specialization of the class template `declarations` names, its
        /// arguments after it.
        Specialization,
        /// A template argument: the integer `value` of the type after it.
        Integral,
        /// A template argument: the template `declarations` names.
        Template,
        /// A template argument: a pack of the arguments after it.
        Pack,
    };

    /// One declaration a name is found through: its name, empty for an
    /// anonymous namespace, and its clang::Decl::Kind.
    struct Declaration {
        std::string name;
        std::uint32_t kind = 0;
    };

    Kind kind = Kind::Builtin;

    /// A type's CVR qualifiers (clang::Qualifiers::getCVRQualifiers).
    std::uint8_t qualifiers = 0;

    std::int64_t value = 0;

    /// How many parts it is made of, those right after it.
    std::uint32_t parts = 0;

    /// A function type's, the qualifiers and the ref-qualifier
    /// (clang::RefQualifierKind) of a member function's among them.
    bool variadic = false;
    bool nothrow = false;
    std::uint8_t method_qualifiers = 0;
    std::uint8_t ref_qualifier = 0;

    /// The declarations from one in the translation unit down to the named
    /// one itself.
    std::vector<Declaration> declarations;
};

/// What the focus file's whole compile tells completion, whose own parse
/// stops at the point: the arguments of the only instantiation of each
/// function template written there, generic lambdas among them, and the
/// class each `auto` variable of a range-based `for` in a template holds.
struct InstantiatedTemplates {
    struct Entry {
        /// Where the name of the template or the variable is in the focus
        /// file, and the name.
        std::uint32_t offset = 0;
        std::string name;

        /// The arguments, one after the other, or the class.
        std::vector<TypePart> parts;
    };

    std::vector<Entry> functions;
    std::vector<Entry> loops;
};

/// What `unit`, a compile of the whole focus file, instantiates the file's
/// templates with (InstantiatedTemplates).
auto instantiated_templates(CompilationUnit& unit) -> InstantiatedTemplates;

/// A completion with `instantiated` from the latest compile of the whole
/// file, which may predate its text: an entry serves the template or the
/// variable of its name at its offset alone.
auto code_complete(CompilationParams params,
                   const CodeCompletionOptions& options = {},
                   const DocumentationOptions& documentation = {},
                   const CompletionClient& client = {},
                   PositionEncoding encoding = PositionEncoding::UTF16,
                   const CompileCompletion& names = {},
                   const InstantiatedTemplates& instantiated = {}) -> Completion;

/// The identifier being typed at `offset` of `text`, from its start to
/// `offset`, and the scope a qualifier before it names.
struct TypedName {
    llvm::StringRef prefix;
    index::CompletionScope scope;

    /// After `.`: a member, which Sema reads from its class whole. Not after
    /// `->`, which a trailing return type follows too.
    bool member = false;
};

/// None in a number, and after a qualifier spelled other than by names
/// (`T<int>::`, `decltype(x)::`).
auto typed_name(llvm::StringRef text, std::uint32_t offset, const clang::LangOptions& dialect)
    -> std::optional<TypedName>;

/// The reply listing `items` under `code_completion.limit`: the best ranked
/// stay, ties by label, and a list cut short is marked incomplete.
auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList;

/// The most names one completion offers from the index (ScopeCandidate).
constexpr inline std::size_t scope_candidate_limit = 100;

/// The names the index offers where `typed` is completed in `file`
/// (ScopeCandidate), appended to `candidates`; whether they are all of
/// them.
bool scope_candidates(const index::IndexQuery& query,
                      const DependencyGraph& graph,
                      const TypedName& typed,
                      Fid file,
                      std::vector<ScopeCandidate>& candidates);

/// A completion's reply finished (Completion): an item naming what the
/// file does not include yet takes the `#include` of the header declaring
/// it, at `include`; one whose header is unknown goes. The
/// list is then cut to `limit`.
auto finish_completion(protocol::CompletionList list,
                       const IncludePlace& include,
                       llvm::ArrayRef<ScopeCandidate> candidates,
                       const index::IndexQuery& query,
                       Fid file,
                       const SearchConfig& search,
                       vfs::Scope& scope,
                       std::uint32_t limit) -> protocol::CompletionList;

/// The paths an `#include` being typed (`completing`) may go on with in
/// `text`, the file at `path`, as `search` finds them: a directory leaves
/// the path open for the `/` typed next, a header closes the directive.
auto include_completion(const CompletionName& completing,
                        llvm::StringRef text,
                        const PositionMap& map,
                        PositionEncoding encoding,
                        llvm::StringRef path,
                        const SearchConfig& search,
                        vfs::Scope& scope,
                        std::uint32_t limit) -> protocol::CompletionList;

/// The directives a directive name being typed (`completing`) may name,
/// as a compile's completion offers them: `#elif`, `#else` and `#endif`
/// inside a conditional block (CompletionName::conditional). A directive's
/// arguments come as placeholders with statement snippets.
auto directive_completion(const CompletionName& completing,
                          const PositionMap& map,
                          PositionEncoding encoding,
                          const clang::LangOptions& dialect,
                          const CodeCompletionOptions& options,
                          const CompletionClient& client) -> protocol::CompletionList;

/// The modules an `import` or a module declaration being typed
/// (`completing`) in `text`, the file at `path`, may name: the unit's own
/// partitions first, then the modules whose interface lies nearest the
/// file — the fewest directories up from it to one holding the interface.
auto module_completion(const CompletionName& completing,
                       llvm::StringRef text,
                       const PositionMap& map,
                       PositionEncoding encoding,
                       llvm::StringRef path,
                       const DependencyGraph& graph,
                       FileTable& files,
                       std::uint32_t limit) -> protocol::CompletionList;

/// A completion request as the server has it before any compile.
struct CompletionInputs {
    /// The file completed in, as the project names it, its path and text,
    /// and the point completed at in the client's encoding.
    Fid file;
    llvm::StringRef path;
    llvm::StringRef text;
    const PositionMap& positions;
    protocol::Position position;
    PositionEncoding encoding;

    /// The character whose typing asked; empty for a request the user made.
    llvm::StringRef trigger_character;

    /// The dialect the file is lexed in.
    const clang::LangOptions& dialect;

    const CodeCompletionOptions& options;
    const CompletionClient& client;

    /// The project: the index offers the names of other scopes, the
    /// dependency graph the module names.
    const index::IndexQuery& query;
    const DependencyGraph& graph;
    FileTable& files;

    /// The header search of the file's command, asked only where an
    /// include path is completed.
    llvm::function_ref<SearchConfig()> search;
    vfs::Scope& scope;
};

/// A trigger character opened nothing to complete: the client gets no
/// items.
struct NoCompletion {};

/// How a completion request is answered: not at all, before any compile,
/// or by a compile of the code.
using CompletionStart = std::variant<NoCompletion, protocol::CompletionList, CompileCompletion>;

/// The completion at a point, as far as the server answers it alone. A
/// client without request-side gating (nvim, zed) forwards every keystroke
/// of an advertised trigger character: space is advertised only so that
/// `import ` opens module suggestions, the others for include paths and
/// member or scope access, and any other use of them completes nothing. A
/// directive name, an include path and a module name are completed here;
/// other code by a compile, with the names the index offers where a name
/// is typed.
auto start_completion(const CompletionInputs& inputs) -> CompletionStart;

}  // namespace clice::feature
