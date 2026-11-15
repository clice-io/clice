module clice:settings.settings;

import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.documentation;
import :feature.hover;
import :feature.inlay_hints;
import :feature.semantic_tokens;
import :feature.signature_help;
import :support.json;
import :support.prelude;

namespace clice {

/// The worker counts "auto" stands for (WorkersSettings): half the
/// physical cores, at least 2, for the background index; the physical cores
/// in all.
std::uint32_t auto_background_count();
std::uint32_t auto_max_count();

/// A cache directory keys its indexes, configuration selection and server
/// record by configuration alone, so it serves one project, whose root it
/// records. One inside a project root belongs to that root whatever it
/// records; one outside (a shared location) to the root it records, while
/// that exists.
std::string cache_dir_owner(llvm::StringRef cache_dir);

/// Whether `cache_dir` serves another project than `project_root`'s.
bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef project_root);

/// Record `project_root`, spelled `shown` as its user names it, as the
/// owner of a cache directory it may use (owned_elsewhere is false); called
/// under the directory's writer lock.
void set_cache_dir_owner(llvm::StringRef cache_dir,
                         CanonicalRef project_root,
                         llvm::StringRef shown);

/// The configuration files a project root may hold, in lookup order.
constexpr inline std::array<llvm::StringRef, 2> settings_file_names = {"clice.toml",
                                                                       ".clice/config.toml"};

/// A compile command written by hand: one string tokenized like a shell
/// command line, or an argv array.
using CommandSpelling = std::variant<std::string, std::vector<std::string>>;

/// How open files are served (`readonly`): whether an open document is
/// compiled, PCH and AST, or its reads answered from the index alone
/// (ServedDocument::compiles). Every request is answered by the best source
/// available at that moment (ServedProject::pick_route). Builds are always
/// pull-driven — no lifecycle event starts one, the first request that
/// needs the AST does.
enum class ReadonlyMode : std::uint8_t {
    /// Every open file targets a full AST; the index answers while the
    /// pulled compile is in flight.
    Off,
    /// Never build a PCH: reads serve from the index alone (a cold file
    /// jumps the indexing queue), while completion and signature help
    /// still compile on demand — without a preamble. The
    /// low-resource profile.
    On,
    /// Files start as On and switch to Off at the first edit intent
    /// (edit, completion, signature help, a context switch, a restored
    /// buffer that diverged from the index). A file the index can never
    /// serve — kept out of the background index, or the index attempt its
    /// opening asked for finished without a servable shard — falls back to
    /// Off rather than answering empty forever.
    Auto,
};

/// A file-pattern rule: where matching files take their compile commands
/// from, how those commands are edited, and how the files are served,
/// indexed, linted and formatted. Corresponds to `[[rules]]` in clice.toml.
/// Every field a rule leaves out it has no say in: of the rules matching a
/// file, the first to set a field decides it (Rules).
KANNOTATE_TYPE(struct Rule, presence_all = optional);
struct Rule {
    KANNOTATE(&patterns,
              description =
                  "Glob patterns selecting the files this rule applies "
                  "to. A relative pattern is anchored at the project root "
                  "(`..` segments allowed); an absolute pattern or one "
                  "starting with `**` matches the file's absolute path. "
                  "`*` matches within a path segment, `?` a single "
                  "character, `**` any number of segments, `{a,b}` "
                  "alternatives, `[0-9]` a character range, `[!...]` a "
                  "negated range, `[*]` a literal `*`. Separators are `/` "
                  "on every platform. Omitted means every file: such a "
                  "rule belongs last, where it decides what no rule "
                  "above it does.")
    std::vector<std::string> patterns;

    KANNOTATE(&configuration,
              description =
                  "Build configuration tag. A tagged rule applies only "
                  "while that configuration is active; an untagged rule "
                  "always applies. The distinct tags form the "
                  "configuration menu; `--configuration`, the persisted "
                  "selection and `default_configuration` pick the active "
                  "one, in that order.")
    std::string configuration;

    KANNOTATE(&compile_commands,
              description =
                  "Compilation databases, in priority order: a "
                  "compile_commands.json or a directory containing one, "
                  "relative to the project root. All of them load, and "
                  "every entry applies to its own file whatever the "
                  "patterns say; the patterns and the order decide which "
                  "entry a file present in several databases gets by "
                  "default. A rule without patterns names the project's "
                  "databases. When no rule declares a source, every "
                  "compile_commands.json in the project root and its "
                  "immediate subdirectories loads, and so do the ones in "
                  "the directories above a file when it is opened; a "
                  "directory below the root with a clice.toml or "
                  ".clice/config.toml of its own is another project, and "
                  "none in it loads.")
    std::vector<std::string> compile_commands;

    KANNOTATE(&resolve_symlinks,
              description =
                  "Whether builds are told apart by the real paths this "
                  "rule's `compile_commands` resolve to, through symlinks "
                  "and Windows junctions, rather than by the "
                  "configuration's name: for a build system that selects "
                  "a build by pointing a link at its directory. Each real "
                  "path keeps an index of its own, and when the link is "
                  "pointed elsewhere the server asks to be restarted on "
                  "that build's index, which still holds what an earlier "
                  "session indexed there. A configuration whose rules differ "
                  "from this configuration's only in naming exactly "
                  "those databases directly shares its index. The index "
                  "of a build no longer used stays in the cache "
                  "directory until deleted.")
    bool resolve_symlinks = false;

    KANNOTATE(&default_command,
              description =
                  "The compile command for matching files without a "
                  "database entry, without the source file: a string "
                  "tokenized like a shell command line, or an argv "
                  "array. It runs from the project root, and the "
                  "matching source files on disk join the background "
                  "index (unless `background_index` is `false`) — "
                  "enumerated at startup and again on every poll, so a "
                  "file created later compiles when opened and joins the "
                  "index within a poll period. A file clang has no "
                  "language for (an Arduino `.ino`, an extensionless "
                  "tool) is a source only when the command forces a "
                  "language (`-x c++`) and a pattern names its suffix "
                  "(`*.ino`) or the file itself. Omitted means none.")
    CommandSpelling default_command;

    KANNOTATE(&append,
              description =
                  "Compilation flags appended for matching files, e.g. "
                  "`[\"-std=c++20\", \"-DNDEBUG\"]`. The flags of every "
                  "matching rule apply, in declaration order.")
    std::vector<std::string> append;

    KANNOTATE(&remove,
              description =
                  "Compilation flags removed for matching files, e.g. "
                  "`[\"-Wall\"]`, before the rule's own `append`. A flag "
                  "with a value removes that value (`-DDEBUG`), the next "
                  "element being a separate value as on a command line; "
                  "one with `*` for its value (`-I*`, `[\"-include\", "
                  "\"*\"]`), or without one at the end of the list "
                  "(`[\"-include\"]`), removes every value; a `*` right "
                  "after a flag's name removes every flag of its kind "
                  "spelled with that prefix (`-W*` takes `-Wall` and "
                  "`-Wextra` too).")
    std::vector<std::string> remove;

    KANNOTATE(&query_toolchain,
              description =
                  "Whether clice runs the compiler of matching files' "
                  "commands to ask how it compiles them: its target, "
                  "standard library and system headers, predefined "
                  "macros and default language standard. `false` runs "
                  "no compiler: clice's own clang reads the command as "
                  "written, with clang's defaults for the compiler's "
                  "name. Unset means `true`.")
    std::optional<bool> query_toolchain;

    KANNOTATE(&background_index,
              description =
                  "Whether the background index compiles matching files "
                  "as translation units of their own. `false` keeps them "
                  "out, and drops what an earlier index recorded of them: "
                  "they still compile when opened, and a header among "
                  "them is still indexed through the units including it. "
                  "A module unit, or the unit writing a precompiled "
                  "header, is indexed all the same by the build of its "
                  "module or header, for the files reading it. A rule "
                  "without patterns saying `false` keeps the background "
                  "index from compiling anything. Unset means `true`.")
    std::optional<bool> background_index;

    KANNOTATE(&skip_function_body,
              description =
                  "Whether the compiles that include matching files skip "
                  "their function bodies. Unset, the editor's compile of "
                  "an open file skips the bodies of the headers it "
                  "includes at its top — several times faster to open "
                  "and rebuild, the errors inside those bodies shown once "
                  "the background index has compiled the file as saved — "
                  "and every other compile reads them. `true`: every "
                  "compile but a matching file's own skips its bodies, "
                  "templates' included, the background index's too, and "
                  "none of them is indexed — the fastest index of code "
                  "you only call, such as third-party libraries, whose "
                  "templates' errors your code causes are then never "
                  "shown. The bodies of templates forwarding their "
                  "arguments (`make_unique`) and of `constexpr` and "
                  "`auto` functions are read all the same. `false`: no "
                  "compile skips them, and their errors show at once.")
    std::optional<bool> skip_function_body;

    KANNOTATE(&index_function_body,
              description =
                  "Whether the index records what the function bodies of "
                  "matching files use, and what their templates' "
                  "instantiations resolve, so that references and the "
                  "call hierarchy reach inside them. Unset means `false` "
                  "for system headers — a library's own internals, which "
                  "only a reader of its code asks about — and `true` for "
                  "every other file. A file opened in the editor is "
                  "indexed whole all the same. Applies to the units "
                  "indexed after a change.")
    std::optional<bool> index_function_body;

    KANNOTATE(&readonly,
              description =
                  "How matching files are served when open: \"off\" "
                  "compiles them — builds are pulled by the first request "
                  "that needs them, with the index answering in the "
                  "meantime; \"on\" builds no preamble PCH — reads serve "
                  "from the index alone (a cold file jumps the indexing "
                  "queue), while completion and signature help still "
                  "compile on demand without a preamble; \"auto\" starts "
                  "a file as \"on\", switches it to \"off\" at the first "
                  "edit intent (edit, completion, signature help, context "
                  "switch), and falls back to \"off\" for a file the "
                  "index cannot serve. Unset means \"off\".")
    std::optional<ReadonlyMode> readonly;

    KANNOTATE(&self_contained,
              description =
                  "Whether matching headers compile on their own. `true`: "
                  "a header compiles alone, without an includer's "
                  "context; `false`: always in the context of a file "
                  "including it. Unset, clice compiles a header alone "
                  "once to find out.")
    std::optional<bool> self_contained;

    KANNOTATE(&other_contexts,
              description =
                  "Whether an open matching file shows, in the code its "
                  "own context skips, what its other contexts compile "
                  "there — the semantic colors, outline, folds, links and "
                  "hover the index holds for them — while it is as the "
                  "index read it. `false` shows its own context alone. "
                  "The editor's choice for a file (Show or Hide other "
                  "contexts) overrides it until the server stops. Unset "
                  "means `true`.")
    std::optional<bool> other_contexts;

    KANNOTATE(&lint,
              description =
                  "Whether `clice lint` checks matching files and the "
                  "editor runs clang-tidy on them. `false` keeps them out: "
                  "their findings are dropped and a translation unit they "
                  "head is not parsed, unless `--index` needs it for the "
                  "index. Files outside the project are never checked. "
                  "Unset means `true`.")
    std::optional<bool> lint;

    KANNOTATE(&format,
              description =
                  "Whether `clice format` formats matching files. "
                  "`false` keeps them out. Files outside the project "
                  "are never formatted. Unset means `true`.")
    std::optional<bool> format;
};

/// Corresponds to the `[workers]` section in clice.toml: the worker
/// processes and when background work starts.
KANNOTATE_TYPE(struct WorkersSettings, presence_all = optional);
struct WorkersSettings {
    KANNOTATE(&foreground,
              minimum = 1,
              description =
                  "Number of foreground workers: they keep the open files "
                  "compiled and answer the requests about them (hover, "
                  "semantic tokens, ...); at least 1.")
    std::uint32_t foreground = 2;

    KANNOTATE(&background,
              description =
                  "The most compiles the background index runs at once, "
                  "never more than `max`: \"auto\", half the machine's "
                  "physical cores and at least 2, or a number, at least 1. "
                  "Raising it speeds a first index up on a big machine.")
    Count background = Automatic::Auto;

    KANNOTATE(&max,
              description =
                  "The most compile workers at once, the background "
                  "index's, completion's, signature help's and the "
                  "precompiled headers' and modules' together: \"auto\", "
                  "the machine's physical cores, or a number, at least 1. "
                  "They start as work needs them and stop after 30 "
                  "seconds idle.")
    Count max = Automatic::Auto;

    KANNOTATE(&background_delay_ms,
              description =
                  "How long the background index waits after the last "
                  "edit or request before it starts, in milliseconds.")
    std::uint32_t background_delay_ms = 3000;

    /// `background` as a number, "auto" the machine's, at most max_count().
    std::uint32_t background_count() const;

    /// `max` as a number: "auto" the machine's.
    std::uint32_t max_count() const;
};

/// Corresponds to the `[references]` section in clice.toml.
KANNOTATE_TYPE(struct ReferencesSettings, presence_all = optional);
struct ReferencesSettings {
    KANNOTATE(&limit,
              description =
                  "Maximum number of locations a references request "
                  "answers with, or `\"unlimited\"`. A reply cut short "
                  "keeps the locations in the file it is asked from "
                  "before any other. `clice query references` lists "
                  "every one.")
    Limit limit = 2000u;
};

/// A rule after finalize(): patterns compiled, paths anchored at the
/// project root.
struct CompiledRule {
    struct Pattern {
        /// Matches the canonical absolute path: a relative pattern was
        /// anchored at the project root when compiled.
        kotatsu::GlobPattern glob;

        /// The pattern `glob` compiled, for a process compiling it again.
        std::string text;

        /// The literal directory the pattern starts in (the project root
        /// for `**`-led patterns): where the files it covers are enumerated.
        CanonicalPath root;
    };

    std::vector<Pattern> patterns;
    std::string configuration;
    /// Absolute paths of the declared databases, in priority order; an
    /// existing directory resolved to the compile_commands.json under it.
    std::vector<Spelling> compile_commands;
    bool resolve_symlinks = false;
    /// The command's argv (a string spelling tokenized with the host's
    /// shell rules), variables substituted; empty means none.
    std::vector<std::string> default_command;
    std::vector<std::string> append;
    std::vector<std::string> remove;
    std::optional<bool> query_toolchain;
    std::optional<bool> background_index;
    std::optional<bool> skip_function_body;
    std::optional<bool> index_function_body;
    std::optional<ReadonlyMode> readonly;
    std::optional<bool> self_contained;
    std::optional<bool> other_contexts;
    std::optional<bool> lint;
    std::optional<bool> format;

    /// Every pattern failed to compile: the rule matches no file, but the
    /// sources it declares still load.
    bool unmatchable = false;

    /// Where the rule is written, as `clice command` names it: the file and
    /// line, or the editor's setting and the rule's index there.
    std::string origin;

    bool has_default_command() const;

    /// Whether the rule declares a command source — databases, or a default
    /// command the rule can actually hand out — rather than only editing
    /// commands.
    bool declares_sources() const;

    /// Whether the rule applies to `path`.
    bool matches(CanonicalRef path) const;

    /// Whether the rule applies while `configuration` is active: it is
    /// untagged or tagged so.
    bool active_under(llvm::StringRef configuration) const;
};

/// A problem found while reading a configuration, carrying enough
/// structure to publish an LSP diagnostic on the file's URI.
struct SettingsIssue {
    enum class Severity : std::uint8_t {
        /// A value clice cannot honor: nothing runs on the configuration.
        Error,
        /// The configuration still applies (e.g. an unknown key was ignored).
        Warning,
    };

    Severity severity;
    /// Absolute path of the configuration file; empty for a problem of the
    /// overlay, whose message names it, of the `--configuration` asked for,
    /// or of the cache directory.
    std::string file;
    std::string message;
    /// 1-based position in the file; 0 when unknown.
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    /// Where the key or value at the position ends, past its last
    /// character; the position itself for a point (a syntax error).
    std::uint32_t end_line = 0;
    std::uint32_t end_column = 0;
};

/// Configuration for the clice LSP server, loadable from clice.toml
/// or passed via LSP initializationOptions.
///
/// A default-constructed Settings is fully valid: every option holds its
/// real default (single source: the field initializers, including the
/// feature options structs, which double as their config sections).
/// Loading is layering (load_settings) — each source is decoded onto the
/// same object in precedence order (clice.toml, then initializationOptions)
/// and only touches the fields it names, nested sections merging per field.
/// finalize() never fills option defaults; it only computes derived
/// values from the merged result.
KANNOTATE_TYPE(struct Settings, presence_all = optional);
struct Settings {
    KANNOTATE(&default_configuration,
              description =
                  "The build configuration to fall back on when neither "
                  "`--configuration` nor a persisted selection names one: "
                  "a tag declared on rules; a name no rule declares is an "
                  "error. Unset, the first declared tag is used.")
    std::string default_configuration;

    /// finalize() leaves it the directory's identity, empty for none;
    /// cache_identity names the directory.
    KANNOTATE(&cache_dir,
              description =
                  "Directory for the unified on-disk cache (PCHs, PCMs and "
                  "the index), relative to the project root. Empty "
                  "defaults to `${workspace}/.clice`, which keeps itself "
                  "out of version control and backups via generated "
                  ".gitignore and CACHEDIR.TAG markers (a "
                  "`.clice/config.toml` stays visible to Git; backup tools "
                  "honoring CACHEDIR.TAG skip the whole directory); an "
                  "explicitly configured directory is never marked. It "
                  "must be on a local disk: on a network file system "
                  "(NFS, SMB, a Windows drive under WSL) the index is not "
                  "saved. The resolved path is printed at startup.")
    std::string cache_dir;

    /// Whether finalize() derived cache_dir rather than the user setting
    /// it. Only such a dedicated root receives the self-ignore markers —
    /// a configured directory may be shared with other content.
    KANNOTATE(&cache_dir_defaulted, skip = true)
    bool cache_dir_defaulted = false;

    /// The identity of cache_dir, empty for none: finalize() resolves it
    /// with cache_dir.
    KANNOTATE(&cache_identity, skip = true)
    CanonicalPath cache_identity;

    KANNOTATE(&logging_dir,
              description =
                  "Directory for log files, relative to the project root; "
                  "empty derives `${cache_dir}/logs`. Each server, "
                  "`clice index` and `clice lint` run logs into a "
                  "subdirectory of its own; the 20 latest are kept.")
    std::string logging_dir;

    KANNOTATE(&poll_seconds,
              description =
                  "Longest interval in seconds between two background "
                  "looks at a project file: the interval doubles at every "
                  "look that finds the file unchanged, up to this. 0 "
                  "disables background polling, compilation databases "
                  "included.")
    std::uint32_t poll_seconds = 30;

    KANNOTATE(&workers, description = "The [workers] section: the worker processes.")
    WorkersSettings workers;

    // The feature sections are the features' own options: their optional
    // presence lets a decode leave unmentioned fields at their initializers,
    // so those are the single source of every default and a settings source
    // only ever overlays what it names.

    KANNOTATE(&diagnostics, description = "The [diagnostics] section: diagnostic options.")
    feature::DiagnosticsOptions diagnostics;

    KANNOTATE(&documentation,
              description = "The [documentation] section: how documentation comments are read.")
    feature::DocumentationOptions documentation;

    KANNOTATE(&hover, description = "The [hover] section: hover rendering options.")
    feature::HoverOptions hover;

    KANNOTATE(&document_symbols,
              description = "The [document_symbols] section: what the outline lists.")
    feature::DocumentSymbolsOptions document_symbols;

    KANNOTATE(&inlay_hints, description = "The [inlay_hints] section: inlay hint options.")
    feature::InlayHintsOptions inlay_hints;

    KANNOTATE(&semantic_tokens,
              description =
                  "The [semantic_tokens] section: what the semantic tokens "
                  "hold and the names they are sent by.")
    feature::SemanticTokensOptions semantic_tokens;

    KANNOTATE(&code_completion,
              description = "The [code_completion] section: code completion options.")
    feature::CodeCompletionOptions code_completion;

    KANNOTATE(&signature_help,
              description = "The [signature_help] section: signature help options.")
    feature::SignatureHelpOptions signature_help;

    KANNOTATE(&references, description = "The [references] section: what find references answers.")
    ReferencesSettings references;

    KANNOTATE(&rules,
              description =
                  "File-pattern rules: where files take their commands "
                  "from and how they are compiled, served, indexed, "
                  "linted and formatted ([[rules]] in clice.toml). Rules "
                  "given through initializationOptions replace the file's.")
    std::vector<Rule> rules;

    /// Whether `rules` came from initializationOptions: they apply to every
    /// project of the editor, a project without C/C++ files among them.
    KANNOTATE(&rules_from_editor, skip = true)
    bool rules_from_editor = false;

    KANNOTATE(&compiled_rules, skip = true)
    std::vector<CompiledRule> compiled_rules;

    /// Compute the values derived from the final merged settings: default
    /// cache/logging directories, variable substitution, path
    /// canonicalization and anchoring at the project root, and rule
    /// compilation. Run once per load, after every source has been
    /// overlaid. Returns the values clice cannot honor, each at its key: a
    /// glob that does not compile or holds a backslash, an environment
    /// variable that is not set, `default_configuration` naming no declared
    /// tag, a zero worker count or limit (each falls back to its default
    /// meanwhile, so the process-wide settings read from it stay runnable).
    std::vector<codec::rich_error> finalize(CanonicalRef project_root);

    /// The distinct configuration tags, in first-appearance order.
    llvm::SmallVector<llvm::StringRef> configurations() const;

    /// Whether `configuration` is one of the tags the rules declare.
    bool declares(llvm::StringRef configuration) const;

    /// The configuration active when nothing selects one:
    /// `default_configuration`, else the first declared tag; empty when the
    /// rules declare none.
    llvm::StringRef fallback_configuration() const;

    /// The configuration's JSON schema (draft 2020-12), pretty-printed.
    /// Unknown properties are rejected — the schema-side face of
    /// load_settings's unknown-key warnings.
    static std::expected<std::string, std::string> json_schema();
};

/// What a project's configuration is read with besides its root.
struct LoadSettingsOptions {
    /// A JSON object of clice.toml sections laid over the file, whole or
    /// not at all: the client's initializationOptions, `--config-overlay`.
    std::optional<codec::dyn::Value> overlay;

    /// What the overlay is called in messages.
    llvm::StringRef overlay_name = "initializationOptions";

    /// The build configuration asked for (`--configuration`); empty for
    /// none.
    llvm::StringRef configuration;

    /// Whether a `configuration` no rule declares is an error, as for the
    /// command line, or ignored with guidance, as for the server, whose
    /// argument outlives the edits of the rules.
    bool strict = true;

    /// The cache directories other projects of this process use.
    llvm::ArrayRef<CanonicalPath> taken_cache_dirs;

    /// The root as the user spelled it, for messages; empty shows its
    /// canonical path.
    llvm::StringRef root_name;
};

/// A project's configuration as read: the configuration, the file it came
/// from, the problems found and the build configuration that applies.
struct LoadedSettings {
    /// The layers that decode, merged and finalized: what process-wide
    /// settings are read from even when the configuration has errors.
    Settings settings;

    /// The configuration file found, also one that does not parse; empty
    /// for none.
    std::string file;

    std::vector<SettingsIssue> issues;

    /// The active build configuration: a declared tag, or empty when the
    /// rules declare none.
    std::string configuration;

    /// Whether an issue is an error: nothing may run on the configuration.
    bool broken() const;
};

/// Whether one of `issues` is an error: nothing may run on the settings.
bool has_error(llvm::ArrayRef<SettingsIssue> issues);

/// Read the configuration of the project at `root` (empty for a project
/// without a folder): find its file (clice.toml, else .clice/config.toml,
/// either anchoring its relative paths at `root`), decode it, lay the
/// overlay over it, finalize and check every value;
/// choose the cache directory — the one the merged configuration names,
/// else the one the file names, else the default, else none, passing over
/// those taken (LoadSettingsOptions::taken_cache_dirs) or owned by another
/// project (owned_elsewhere); then resolve the build configuration from
/// that directory's selection. A layer that does not decode is left out;
/// a key clice 1.0 moved is an error naming its new place, any other
/// unknown key a warning, and the rest of its layer applies.
LoadedSettings load_settings(CanonicalRef root, const LoadSettingsOptions& options = {});

}  // namespace clice

/// The settings spell a mode in lowercase (`off`).
template <>
struct kotatsu::codec::repr<clice::ReadonlyMode> : clice::LowercaseNames {};
