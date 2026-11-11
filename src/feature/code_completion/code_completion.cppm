module clice:feature.code_completion;

import :command.search_config;
import :compile.compilation;
import :feature.code_completion.cursor;
import :feature.documentation;
import :index.query;
import :support.json;
import :support.prelude;
import :syntax.dependency_graph;
import :syntax.position;
import :syntax.scan;
import :syntax.source_text;
import :vfs.dir_cache;
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
KANNOTATE_TYPE(struct CodeCompletionOptions, presence_all = optional);
struct CodeCompletionOptions {
    KANNOTATE(&arguments,
              with = type<LowercaseNames>,
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
                  "parentheses of a call alone. An Objective-C message "
                  "takes a placeholder or an empty tab stop after each piece "
                  "of its selector, its first piece alone with \"none\" or "
                  "without snippet support.")
    CompletionArguments arguments = CompletionArguments::Placeholders;

    KANNOTATE(&bundle_overloads,
              description = "Collapse an overload set into a single completion item.")
    bool bundle_overloads = true;

    KANNOTATE(&statement_snippets,
              description =
                  "Complete statements such as `if` and `for` as snippets "
                  "with placeholders for their parts; otherwise only the "
                  "keyword is inserted. Ignored for clients without "
                  "snippet support.")
    bool statement_snippets = false;

    KANNOTATE(&dot_to_arrow,
              description =
                  "After `.` on a pointer, list the members `->` reaches and "
                  "turn the `.` into `->` when one is chosen.")
    bool dot_to_arrow = true;

    KANNOTATE(&all_scopes,
              description =
                  "Where an unqualified name is being typed, also offer "
                  "the project's names of other namespaces it matches, "
                  "inserting the qualifier they need; one in several "
                  "namespaces is offered once per namespace. Names the "
                  "file reaches already are offered as they are; for the "
                  "others see `insert_includes` and `insert_imports`. "
                  "The names of other namespaces come from the index.")
    bool all_scopes = true;

    KANNOTATE(&insert_includes,
              description =
                  "With `all_scopes`, also offer names declared, and macros "
                  "defined, in headers the file does not include yet, adding "
                  "the `#include` of the header that holds them.")
    bool insert_includes = true;

    KANNOTATE(&insert_imports,
              description =
                  "With `all_scopes`, also offer names exported by modules "
                  "the file does not import yet, adding the `import`. Where "
                  "the project builds the `std` module, a file that imports "
                  "modules or is a module unit takes the standard library's "
                  "names with `import std;` rather than their headers.")
    bool insert_imports = true;

    KANNOTATE(&limit,
              description =
                  "Maximum number of completion items, the best ranked; "
                  "`0` means no limit. A list cut short is marked "
                  "incomplete, so the editor asks again as you type.")
    std::uint32_t limit = 100;
};

/// What the editor's completion supports, read once from the LSP
/// capabilities it sends at the handshake (`completionItem.snippetSupport`,
/// `insertReplaceSupport`, `documentationFormat`), whose nested optionals
/// the completion and the worker computing it need not read again.
struct CompletionClient {
    /// Insert texts may carry placeholders.
    bool snippets = false;

    /// An item may carry both an insert and a replace range; the editor
    /// picks one by its own setting.
    bool insert_replace = false;

    /// Documentation may be markdown rather than plain text.
    bool markdown = false;
};

/// A name completion may offer from the project's index: the completion
/// looks it up to tell whether the file reaches
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

    /// A standard library name the project's `std` module exports: a file
    /// using modules imports it rather than including its header.
    bool std_module = false;

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

/// What a compile of the code completes with (code_complete): the names of
/// other scopes the index offers for the name typed (ScopeCandidate), and
/// whether it holds more of them.
struct CompileCompletion {
    std::vector<ScopeCandidate> candidates;
    bool more_candidates = false;
};

/// The completion at the point of `params`. Of the files the compile loads,
/// Sema reads the names the typed one matches where it lists names
/// (TypedNames), and all of them where nothing is typed yet.
auto code_complete(CompilationParams params,
                   const CodeCompletionOptions& options,
                   const DocumentationOptions& documentation,
                   const CompletionClient& client,
                   PositionEncoding encoding = PositionEncoding::UTF16,
                   const CompileCompletion& names = {}) -> Completion;

/// The most names one completion takes from the index (ScopeCandidate), and
/// reads of the files its compile loads where a name is typed (TypedNames).
constexpr inline std::size_t scope_candidate_limit = 100;

/// A completion's reply finished (Completion): an item naming what the
/// file does not include yet takes the `#include` of the header declaring
/// it, at `include`; one whose header is unknown goes. The list is then cut
/// to `limit`.
auto finish_completion(protocol::CompletionList list,
                       const IncludePlace& include,
                       llvm::ArrayRef<ScopeCandidate> candidates,
                       const index::IndexQuery& query,
                       const DependencyGraph& graph,
                       Fid file,
                       const SearchConfig& search,
                       std::uint32_t limit) -> protocol::CompletionList;

/// The buffer of an open translation unit, which may declare another module
/// than the graph knows until the file is saved, or one where the graph
/// knows none.
struct OpenUnit {
    Fid file;
    llvm::StringRef text;
};

/// A completion request as the server has it before any compile.
struct CompletionInputs {
    /// The file completed in and its text, read in the file's dialect.
    Fid file;
    SourceText& source;

    /// The request, its positions in `encoding`, the one the client and the
    /// server agreed on.
    const protocol::CompletionParams& params;
    PositionEncoding encoding;

    const CodeCompletionOptions& options;
    const CompletionClient& client;

    /// The project: the dependency graph names the modules.
    const DependencyGraph& graph;

    /// The header search of the file's command, asked only where an
    /// include path is completed.
    llvm::function_ref<SearchConfig()> search;

    /// The module declaration of the text given, read by the preprocessor
    /// under the file's command: asked where an import is completed below
    /// a declaration a conditional holds.
    llvm::function_ref<ScanResult(llvm::StringRef)> declared_module;

    /// The buffers of the project's other open translation units, asked
    /// where a module name is completed.
    llvm::function_ref<std::vector<OpenUnit>()> open_units;

    /// The project's index: the names it offers a compile, the
    /// documentation of the modules a module name completes with.
    const index::IndexQuery& query;
    const DocumentationOptions& documentation;
};

/// A trigger character opened nothing to complete: the client gets no
/// items.
struct NoCompletion {};

/// The completion at a point, as far as the server answers it alone. A
/// client without request-side gating (nvim, zed) forwards every keystroke
/// of an advertised trigger character: space is advertised only so that
/// `import ` opens module suggestions, the others for include paths and
/// member or scope access, and any other use of them completes nothing. A
/// directive name, an include path and a module name are completed here;
/// other code by a compile, given the index's names for the name typed:
/// those the file reaches, and those it reaches once it includes or imports
/// what declares them.
auto code_complete(const CompletionInputs& inputs)
    -> std::variant<NoCompletion, protocol::CompletionList, CompileCompletion>;

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

/// The modules an `import` or a module declaration being typed
/// (`completing`) may name, each with its documentation: the better the
/// name typed matches, the earlier; then the unit's own partitions first,
/// then the modules whose interface lies nearest the file — the fewest
/// directories up from it to one holding the interface.
/// The other open units provide what their buffers declare, the file what
/// its text does.
auto module_completion(const CompletionName& completing, const CompletionInputs& inputs)
    -> protocol::CompletionList;

/// Completion items rank alike wherever they come from: a compile's
/// candidates, the directives, the include paths and the module names.
///
/// How strongly the context asks for a candidate, apart from how its name
/// matches: clang's priority — locals before members before the rest, a
/// type the context expects ahead of all, macros last — as a factor around
/// 1, the priority of a keyword.
inline float priority_weight(unsigned priority) {
    return std::max(0.05F, (90.0F - static_cast<float>(priority)) / 50.0F);
}

/// Ascending sort text for a candidate scoring `score`, at most 8; `order`
/// breaks ties.
inline std::string sort_text(float score, std::optional<std::uint32_t> order) {
    auto text = std::format("{:.4f}", 8.0F - score);
    if(order) {
        text += std::format("{:05}", *order);
    }
    return text;
}

/// The reply listing `items` under `code_completion.limit`: the best ranked
/// stay, ties by label, and a list cut short is marked incomplete.
inline auto completion_list(std::vector<protocol::CompletionItem> items, std::uint32_t limit)
    -> protocol::CompletionList {
    protocol::CompletionList list{.is_incomplete = false, .items = std::move(items)};
    if(limit != 0 && list.items.size() > limit) {
        ranges::sort(list.items, {}, [](const protocol::CompletionItem& item) {
            return std::tie(static_cast<const std::optional<std::string>&>(item.sort_text),
                            item.label);
        });
        list.items.resize(limit);
        list.is_incomplete = true;
    }
    return list;
}

/// Text a snippet takes literally: `$` and `\` would otherwise start or
/// escape a snippet construct, and inside a placeholder `}` would end it.
inline auto escape_snippet(llvm::StringRef text, bool placeholder = false) -> std::string {
    std::string escaped;
    for(char c: text) {
        if(c == '$' || c == '\\' || (placeholder && c == '}')) {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped;
}

}  // namespace clice::feature
