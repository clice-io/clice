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

/// Defaults that are computed rather than written: a fresh Settings queries
/// them in its field initializers, so a default-constructed Settings is
/// already fully valid ("born valid") and no later pass fills options in.
std::uint32_t default_stateless_worker_count();
std::uint32_t default_max_stateless_worker_count();

/// A cache directory keys its indexes, configuration selection and server
/// record by configuration alone, so it serves one project, whose root it
/// records. One inside a project root belongs to that root whatever it
/// records; one outside (a shared location) to the root it records, while
/// that exists.
std::string cache_dir_owner(llvm::StringRef cache_dir);

/// Whether `cache_dir` serves another project than `project_root`'s.
bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef project_root);

/// Record `project_root` as the owner of a cache directory it may use
/// (owned_elsewhere is false); called under the directory's writer lock.
void set_cache_dir_owner(llvm::StringRef cache_dir, CanonicalRef project_root);

/// The configuration files a project root may hold, in lookup order.
constexpr inline std::array<llvm::StringRef, 2> settings_file_names = {"clice.toml",
                                                                       ".clice/config.toml"};

/// A compile command written by hand: one string tokenized like a shell
/// command line, or an argv array.
using CommandSpelling = std::variant<std::string, std::vector<std::string>>;

/// A file-pattern rule: where matching files take their compile commands
/// from and how those commands are edited. Corresponds to `[[rules]]` in
/// clice.toml.
struct Rule {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Glob patterns selecting the files this rule applies "
                         "to. A relative pattern is anchored at this "
                         "configuration file's directory (`..` segments "
                         "allowed), or at the project root for a rule passed "
                         "through initializationOptions; an absolute pattern "
                         "or one starting with `**` matches the file's "
                         "absolute path. "
                         "`*` matches within a path segment, `?` a single "
                         "character, `**` any number of segments, `{a,b}` "
                         "alternatives, `[0-9]` a character range, `[!...]` a "
                         "negated range. Omitted means every file.")
    <std::vector<std::string>> patterns;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Build configuration tag. A tagged rule applies only "
                         "while that configuration is active; an untagged rule "
                         "always applies. The distinct tags form the "
                         "configuration menu; `--configuration`, the persisted "
                         "selection and `default_configuration` pick the active "
                         "one, in that order.")
    <std::string> configuration;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation databases, in priority order: a "
                         "compile_commands.json or a directory containing one, "
                         "relative to this configuration file (to the "
                         "project root for a rule passed through "
                         "initializationOptions). All of them load, and every "
                         "entry applies to its own file "
                         "whatever the patterns say; the patterns and the "
                         "order decide which entry a file present in several "
                         "databases gets by default. A rule without patterns "
                         "names the project's databases. When no rule "
                         "declares a source, every compile_commands.json in "
                         "the project root and its immediate subdirectories "
                         "loads, and so do the ones in the directories above "
                         "a file when it is opened.")
    <std::vector<std::string>> compile_commands;

    KOTATSU_ANNOTATE(defaulted = true,
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
    <bool> resolve_symlinks = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The compile command for matching files without a "
                         "database entry, without the source file: a string "
                         "tokenized like a shell command line, or an argv "
                         "array. It runs from the directory of the configuration "
                         "file it was read from (the project root for a rule "
                         "passed through initializationOptions), and the "
                         "matching source files on disk "
                         "join the background index (unless the rule turns "
                         "`index` off) — enumerated at startup and again on "
                         "every workspace poll, so a file created later "
                         "compiles when opened and joins the index within a "
                         "poll period. A file clang has no language for (an "
                         "Arduino `.ino`, an extensionless tool) is a source "
                         "only when the command forces a language (`-x c++`) "
                         "and a pattern names its suffix (`*.ino`) or the file "
                         "itself. Omitted means none.")
    <CommandSpelling> default_command;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation flags appended for matching files, e.g. "
                         "`[\"-std=c++20\", \"-DNDEBUG\"]`.")
    <std::vector<std::string>> append;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation flags removed for matching files, e.g. "
                         "`[\"-Wall\"]`. A flag with a value removes that value "
                         "(`-DDEBUG`), the next element being a separate value "
                         "as on a command line; one with `*` for its value "
                         "(`-I*`, `[\"-include\", \"*\"]`), or without one "
                         "at the end of the list (`[\"-include\"]`), removes "
                         "every value; a `*` right after a flag's name removes "
                         "every flag of its kind spelled with that prefix "
                         "(`-W*` takes `-Wall` and `-Wextra` too).")
    <std::vector<std::string>> remove;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether clice runs the compiler of matching files' "
                         "commands to ask how it compiles them: its target, "
                         "standard library and system headers, predefined "
                         "macros and default language standard. `false` runs "
                         "no compiler: clice's own clang reads the command as "
                         "written, with clang's defaults for the compiler's "
                         "name. Any matching rule saying `false` wins.")
    <bool> query_toolchain = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether matching translation units join the "
                         "background index. `false` keeps them out; they still "
                         "compile when opened and still host the headers they "
                         "include. Any matching rule saying `false` wins.")
    <bool> index = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether `clice lint` checks matching files. `false` "
                         "keeps them out: their findings are dropped and a "
                         "translation unit they head is not parsed, unless "
                         "`--index` needs it for the index. Files outside the "
                         "project are never checked. Any matching rule "
                         "saying `false` wins.")
    <bool> lint = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether `clice format` formats matching files. "
                         "`false` keeps them out. Files outside the project "
                         "are never formatted. Any matching rule saying "
                         "`false` wins.")
    <bool> format = true;

    /// Where the rule's relative paths and patterns anchor and its default
    /// command runs: the directory of the configuration file it was read
    /// from; empty for a rule from initializationOptions, which anchors at
    /// the project root.
    KOTATSU_ANNOTATE(skip = true)
    <std::string> directory;
};

/// How open files are served (`readonly`). Routing is not governed by this:
/// every request is answered by the best source available at that moment
/// (see ServedProject::pick_route); the mode only decides whether PCH/AST builds are a goal
/// at all. Builds are always pull-driven — no lifecycle event starts one,
/// the first request that needs the AST does.
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
    /// serve — indexing disabled, or its boost attempt finished without a
    /// servable shard — falls back to Off rather than answering empty
    /// forever.
    Auto,
};

/// Corresponds to the `[project]` section in clice.toml. Field
/// initializers are the defaults; `cache_dir` and `logging_dir` stay empty
/// here because their defaults derive from the project root in
/// finalize().
struct ProjectSettings {
    /// finalize() leaves it the directory's identity, empty for none;
    /// cache_identity() names the directory.
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Directory for the unified on-disk cache (PCH, PCM and "
                         "index artifacts). Empty defaults to `${workspace}/.clice`, "
                         "which keeps itself out of version control and backups via "
                         "generated .gitignore and CACHEDIR.TAG markers (a "
                         "`.clice/config.toml` stays visible to Git; backup tools "
                         "honoring CACHEDIR.TAG skip the whole directory); an "
                         "explicitly configured directory is never marked. The "
                         "resolved path is printed at startup.")
    <std::string> cache_dir;

    /// Whether finalize() derived cache_dir rather than the user setting
    /// it. Only such a dedicated root receives the self-ignore markers —
    /// a configured directory may be shared with other content.
    KOTATSU_ANNOTATE(skip = true)
    <bool> cache_dir_defaulted = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Directory for log files; empty derives `${cache_dir}/logs`. "
                         "Each server session logs into its own timestamped "
                         "subdirectory.")
    <std::string> logging_dir;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Build the background index that serves cross-TU "
                         "features (find references, workspace symbols, ...).")
    <bool> enable_indexing = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Index what the standard library's own template "
                         "instantiations and macros resolve inside the library — "
                         "the comparator `std::sort` calls, the destructor "
                         "`std::vector` runs — so references and the call "
                         "hierarchy reach those places. Off by default: they are "
                         "library internals, and walking them costs indexing time "
                         "and space. Applies to the units indexed after a change.")
    <bool> standard_library_instantiations = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Skip the bodies of the functions in the headers an open "
                         "file includes at its top, as clangd does, but those of "
                         "templates forwarding their arguments (`make_unique`) and "
                         "of `constexpr` and `auto` functions, which clang always "
                         "parses. The file opens and rebuilds after an edit several "
                         "times faster and holds less memory; the price is the "
                         "errors and warnings inside those bodies, a header "
                         "template's instantiated from the file among them, and "
                         "whether a declaration is unused, which show only when "
                         "the background index has compiled the file as it is "
                         "saved. The background index and every command but "
                         "`clice inspect` still compile the bodies.")
    <bool> skip_header_bodies = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     enum_string = type<LowercaseName>,
                     description =
                         "Read-only serving for open files: \"off\" targets a "
                         "full AST for every open file — builds are pulled by "
                         "the first request that needs them, with the index "
                         "answering in the meantime; \"on\" builds no "
                         "preamble PCH — reads serve from the index alone (a "
                         "cold file jumps the indexing queue), while completion "
                         "and signature help still compile on demand without a "
                         "preamble; \"auto\" starts every file as \"on\", "
                         "switches it to \"off\" at the first edit intent "
                         "(edit, completion, signature help, context switch), "
                         "and falls back to \"off\" for a file the index "
                         "cannot serve. Feature routing always answers from "
                         "the best source currently available.")
    <ReadonlyMode> readonly = ReadonlyMode::Off;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Idle delay in milliseconds before background indexing "
                         "starts.")
    <std::uint32_t> idle_timeout_ms = 3000;

    /// The hooks can generate load on demand (log floods).
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Enable the clice/internal test hooks used by the test "
                         "harness.")
    <bool> test_hooks = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     minimum = 1,
                     description =
                         "Number of stateful workers — they hold ASTs in memory "
                         "and serve queries (hover, semantic tokens, ...); at "
                         "least 1.")
    <std::uint32_t> stateful_worker_count = 2;

    KOTATSU_ANNOTATE(defaulted = true,
                     schema_default = false,
                     minimum = 1,
                     description =
                         "Number of stateless workers started — they handle "
                         "ephemeral tasks (PCH/PCM builds, completion, signature "
                         "help, background indexing) — and the most that "
                         "background indexing keeps busy at once; defaults to "
                         "half the machine's physical cores, at least 2. At "
                         "least 1.")
    <std::uint32_t> stateless_worker_count = default_stateless_worker_count();

    /// See WorkerPoolOptions.
    KOTATSU_ANNOTATE(defaulted = true,
                     minimum = 1,
                     description =
                         "Lower bound for dynamic stateless-worker scaling; at "
                         "least 1.")
    <std::uint32_t> min_stateless_worker_count = 1;

    KOTATSU_ANNOTATE(defaulted = true,
                     schema_default = false,
                     description =
                         "Upper bound for dynamic stateless-worker scaling, which "
                         "only interactive work that finds every worker busy "
                         "reaches; `0` means the machine's physical cores, which "
                         "is also the default.")
    <std::uint32_t> max_stateless_worker_count = default_max_stateless_worker_count();

    /// The identity of cache_dir, empty for none.
    CanonicalPath cache_identity() const {
        return cache_dir.empty() ? CanonicalPath() : CanonicalPath(Spelling::absolute(cache_dir));
    }
};

/// Corresponds to the `[tracker]` section in clice.toml: how often files
/// are looked at in the background (integration tests drive ticks through
/// the clice/internal/poll hook instead).
struct TrackerSettings {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Longest interval in seconds between two background looks at a "
                         "workspace file: the interval doubles at every look that finds the "
                         "file unchanged, up to this. 0 disables background polling, "
                         "compilation databases included.")
    <std::uint32_t> workspace_poll_seconds = 30;
};

/// Corresponds to the `[references]` section in clice.toml.
struct ReferencesSettings {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Maximum number of locations a references request "
                         "answers with; `0` means no limit. A reply cut short "
                         "keeps the locations in the file it is asked from "
                         "before any other. `clice query references` lists "
                         "every one.")
    <std::uint32_t> limit = 2000;
};

/// A rule after finalize(): patterns compiled, paths anchored.
struct CompiledRule {
    struct Pattern {
        /// Matches the canonical absolute path: a relative pattern was
        /// anchored at the configuration file's directory when compiled.
        kotatsu::GlobPattern glob;

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
    /// shell rules), `${workspace}` substituted; empty means none.
    /// `directory` is its working directory.
    std::vector<std::string> default_command;
    Spelling directory;
    std::vector<std::string> append;
    std::vector<std::string> remove;
    bool query_toolchain = true;
    bool index = true;
    bool lint = true;
    bool format = true;

    /// Every pattern failed to compile: the rule matches no file, but the
    /// sources it declares still load.
    bool unmatchable = false;

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
    /// overlay, whose message names it, or of the `--configuration` asked
    /// for.
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
struct Settings {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The build configuration to fall back on when neither "
                         "`--configuration` nor a persisted selection names one: "
                         "a tag declared on rules; a name no rule declares is an "
                         "error. Unset, the first declared tag is used.")
    <std::string> default_configuration;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [project] section: project-wide server options.")
    <ProjectSettings> project;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [tracker] section: background polling of files.")
    <TrackerSettings> tracker;

    // The feature sections are the features' own options: `defaulted =
    // true` lets a decode leave unmentioned fields at their initializers, so
    // those are the single source of every default and a settings source only
    // ever overlays what it names.

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [diagnostics] section: diagnostic options.")
    <feature::DiagnosticsOptions> diagnostics;

    KOTATSU_ANNOTATE(
        defaulted = true,
        description = "The [documentation] section: how documentation comments are read.")
    <feature::DocumentationOptions> documentation;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [hover] section: hover rendering options.")
    <feature::HoverOptions> hover;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [document_symbols] section: what the outline lists.")
    <feature::DocumentSymbolsOptions> document_symbols;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [inlay_hints] section: inlay hint options.")
    <feature::InlayHintsOptions> inlay_hints;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The [semantic_tokens] section: what the semantic tokens "
                         "hold and the names they are sent by.")
    <feature::SemanticTokensOptions> semantic_tokens;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [code_completion] section: code completion options.")
    <feature::CodeCompletionOptions> code_completion;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [signature_help] section: signature help options.")
    <feature::SignatureHelpOptions> signature_help;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [references] section: what find references answers.")
    <ReferencesSettings> references;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "File-pattern rules that adjust compilation flags "
                         "([[rules]] in clice.toml).")
    <std::vector<Rule>> rules;

    KOTATSU_ANNOTATE(skip = true)
    <std::vector<CompiledRule>> compiled_rules;

    /// Compute the values derived from the final merged settings: default
    /// cache/logging directories, ${workspace} substitution, path
    /// canonicalization and anchoring, and rule compilation. Run once per
    /// load, after every source has been overlaid. Returns the values clice
    /// cannot honor, each at its key: a glob that does not compile,
    /// `default_configuration` naming no declared tag, a zero worker count
    /// (which falls back to its default meanwhile, so the process-wide
    /// settings read from it stay runnable).
    std::vector<codec::rich_error> finalize(CanonicalRef project_root);

    /// The compiled rules applying to `path`, in declaration order,
    /// restricted to untagged rules and rules tagged `configuration`.
    llvm::SmallVector<const CompiledRule*> matching_rules(CanonicalRef path,
                                                          llvm::StringRef configuration) const;

    /// The distinct configuration tags, in first-appearance order.
    llvm::SmallVector<llvm::StringRef> configurations() const;

    /// Whether `configuration` is one of the tags the rules declare.
    bool declares(llvm::StringRef configuration) const;

    /// The configuration active when nothing selects one:
    /// `default_configuration`, else the first declared tag; empty when the
    /// rules declare none.
    llvm::StringRef fallback_configuration() const;

    /// The databases the rules active under `configuration` declare, in
    /// declaration order, each once.
    llvm::SmallVector<Spelling> declared_databases(llvm::StringRef configuration) const;

    /// The configuration's JSON schema (draft 2020-12), pretty-printed.
    /// Fields whose defaults derive from the running machine (the worker
    /// counts follow the CPU count) carry no `default` annotation, so the
    /// schema is byte-identical on every host. Unknown properties are
    /// rejected — the schema-side face of load_settings's unknown-key
    /// warnings.
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
/// without a folder): find its file (clice.toml, else .clice/config.toml),
/// decode it, lay the overlay over it, finalize and check every value;
/// choose the cache directory — the one the merged configuration names,
/// else the one the file names, else the default, else none, passing over
/// those taken (LoadSettingsOptions::taken_cache_dirs) or owned by another
/// project (owned_elsewhere); then resolve the build configuration from
/// that directory's selection. A layer that does not decode is left out,
/// an unknown key is a warning and the rest of its layer applies.
LoadedSettings load_settings(CanonicalRef root, const LoadSettingsOptions& options = {});

}  // namespace clice
