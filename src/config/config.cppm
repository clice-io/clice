module;

#include "support/prelude.h"

module clice:config.config;

import :feature.code_completion;
import :feature.diagnostics;
import :feature.document_symbols;
import :feature.documentation;
import :feature.hover;
import :feature.inlay_hints;
import :feature.semantic_tokens;
import :feature.signature_help;

namespace clice {

/// Defaults that are computed rather than written: a fresh Config queries
/// them in its field initializers, so a default-constructed Config is
/// already fully valid ("born valid") and no later pass fills options in.
std::uint32_t default_stateless_worker_count();
std::uint32_t default_max_stateless_worker_count();

/// A cache directory keys its indexes, configuration selection and server
/// record by configuration alone, so it serves one project, whose root it
/// records. One inside a workspace root belongs to that root whatever it
/// records; one outside (a shared location) to the root it records, while
/// that exists.
std::string cache_dir_owner(llvm::StringRef cache_dir);

/// Whether `cache_dir` serves another project than `workspace_root`'s.
bool owned_elsewhere(llvm::StringRef cache_dir, CanonicalRef workspace_root);

/// Record `workspace_root` as the owner of a cache directory it may use
/// (owned_elsewhere is false); called under the directory's writer lock.
void set_cache_dir_owner(llvm::StringRef cache_dir, CanonicalRef workspace_root);

/// The configuration files a workspace root may hold, in lookup order.
constexpr inline std::array<llvm::StringRef, 2> config_file_names = {"clice.toml",
                                                                     ".clice/config.toml"};

/// A compile command written by hand: one string tokenized like a shell
/// command line, or an argv array.
using CommandSpelling = std::variant<std::string, std::vector<std::string>>;

/// A file-pattern rule: where matching files take their compile commands
/// from and how those commands are edited. Corresponds to `[[rules]]` in
/// clice.toml.
struct ConfigRule {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Glob patterns selecting the files this rule applies "
                         "to. A relative pattern is anchored at this "
                         "configuration file's directory (`..` segments "
                         "allowed), or at the workspace root for a rule passed "
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
                         "workspace root for a rule passed through "
                         "initializationOptions). All of them load, and every "
                         "entry applies to its own file "
                         "whatever the patterns say; the patterns and the "
                         "order decide which entry a file present in several "
                         "databases gets by default. A rule without patterns "
                         "names the workspace's databases. When no rule "
                         "declares a source, every compile_commands.json in "
                         "the workspace root and its immediate subdirectories "
                         "loads, and so do the ones in the directories above "
                         "a file when it is opened.")
    <std::vector<std::string>> compile_commands;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The compile command for matching files without a "
                         "database entry, without the source file: a string "
                         "tokenized like a shell command line, or an argv "
                         "array. It runs from the directory of the configuration "
                         "file it was read from (the workspace root for a rule "
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
                         "workspace are never checked. Any matching rule "
                         "saying `false` wins.")
    <bool> lint = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether `clice format` formats matching files. "
                         "`false` keeps them out. Files outside the workspace "
                         "are never formatted. Any matching rule saying "
                         "`false` wins.")
    <bool> format = true;

    /// Where the rule's relative paths and patterns anchor and its default
    /// command runs: the directory of the configuration file it was read
    /// from; empty for a rule from initializationOptions, which anchors at
    /// the workspace root.
    KOTATSU_ANNOTATE(skip = true)
    <std::string> directory;
};

/// Corresponds to the `[project]` section in clice.toml. Field
/// initializers are the defaults; `cache_dir` and `logging_dir` stay empty
/// here because their defaults derive from the workspace root in
/// finalize().
struct ProjectConfig {
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
                     choices = {"off", "on", "auto"},
                     description =
                         "Read-only serving for open files: \"off\" targets a "
                         "full AST for every open file — builds are pulled by "
                         "the first request that needs them, with the index "
                         "answering in the meantime; \"on\" never builds a "
                         "PCH — reads serve from the index alone (a cold file "
                         "jumps the indexing queue), while completion and "
                         "signature help still compile on demand without a "
                         "preamble; \"auto\" starts every file as \"on\", "
                         "switches it to \"off\" at the first edit intent "
                         "(edit, completion, signature help, context switch), "
                         "and falls back to \"off\" for a file the index "
                         "cannot serve. Feature routing always answers from "
                         "the best source currently available.")
    <std::string> readonly = "off";

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
                         "and serve queries (hover, semantic tokens, ...); `0` is "
                         "invalid and falls back to the default.")
    <std::uint32_t> stateful_worker_count = 2;

    KOTATSU_ANNOTATE(defaulted = true,
                     schema_default = false,
                     minimum = 1,
                     description =
                         "Number of stateless workers started — they handle "
                         "ephemeral tasks (PCH/PCM builds, completion, signature "
                         "help, background indexing) — and the most that "
                         "background indexing keeps busy at once; defaults to "
                         "half the machine's physical cores, at least 2. `0` is "
                         "invalid and falls back to that default.")
    <std::uint32_t> stateless_worker_count = default_stateless_worker_count();

    /// See WorkerPoolOptions.
    KOTATSU_ANNOTATE(defaulted = true,
                     minimum = 1,
                     description =
                         "Lower bound for dynamic stateless-worker scaling; `0` "
                         "is invalid and falls back to the default.")
    <std::uint32_t> min_stateless_worker_count = 1;

    KOTATSU_ANNOTATE(defaulted = true,
                     schema_default = false,
                     description =
                         "Upper bound for dynamic stateless-worker scaling, which "
                         "only interactive work that finds every worker busy "
                         "reaches; `0` means the machine's physical cores, which "
                         "is also the default.")
    <std::uint32_t> max_stateless_worker_count = default_max_stateless_worker_count();
};

/// Corresponds to the `[tracker]` section in clice.toml: how often files
/// are looked at in the background (integration tests drive ticks through
/// the clice/internal/poll hook instead).
struct TrackerConfig {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Longest interval in seconds between two background looks at a "
                         "workspace file: the interval doubles at every look that finds the "
                         "file unchanged, up to this. 0 disables background polling, "
                         "compilation databases included.")
    <std::uint32_t> workspace_poll_seconds = 30;
};

/// Corresponds to the `[references]` section in clice.toml.
struct ReferencesConfig {
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
        kota::GlobPattern glob;

        /// The literal directory the pattern starts in (the workspace root
        /// for `**`-led patterns): where the files it covers are enumerated.
        CanonicalPath root;
    };

    std::vector<Pattern> patterns;
    std::string configuration;
    /// Absolute paths of the declared databases, in priority order; an
    /// existing directory resolved to the compile_commands.json under it.
    std::vector<Spelling> compile_commands;
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
};

/// A problem found while loading a configuration file, carrying enough
/// structure to publish an LSP diagnostic on the file's URI.
struct ConfigIssue {
    enum class Severity : std::uint8_t {
        /// The configuration was rejected and defaults are in effect.
        Error,
        /// The configuration still applies (e.g. an unknown key was ignored).
        Warning,
    };

    Severity severity;
    /// Absolute path of the configuration file.
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
/// A default-constructed Config is fully valid: every option holds its
/// real default (single source: the field initializers, including the
/// feature options structs, which double as their config sections).
/// Loading is layering — each source is decoded onto the same object in
/// precedence order (clice.toml, then initializationOptions) and only
/// touches the fields it names, nested sections merging per field.
/// finalize() never fills option defaults; it only computes derived
/// values from the merged result.
struct Config {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "The build configuration to fall back on when neither "
                         "`--configuration` nor a persisted selection names one: "
                         "a tag declared on rules. When rules carry tags and this "
                         "names none of them, the first declared tag is used and "
                         "a warning is logged.")
    <std::string> default_configuration;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [project] section: project-wide server options.")
    <ProjectConfig> project;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [tracker] section: background polling of files.")
    <TrackerConfig> tracker;

    // The feature sections are the features' own options: `defaulted =
    // true` lets a decode leave unmentioned fields at their initializers, so
    // those are the single source of every default and a config source only
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
    <ReferencesConfig> references;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "File-pattern rules that adjust compilation flags "
                         "([[rules]] in clice.toml).")
    <std::vector<ConfigRule>> rules;

    KOTATSU_ANNOTATE(skip = true)
    <std::vector<CompiledRule>> compiled_rules;

    /// The workspace root finalize() ran for: the `${workspace}` value, the
    /// anchor of rules and databases no configuration file supplied, and
    /// the enumeration root of `**`-led patterns.
    KOTATSU_ANNOTATE(skip = true)
    <CanonicalPath> workspace_root;

    /// Compute the values derived from the final merged config: default
    /// cache/logging directories, ${workspace} substitution, path
    /// canonicalization and anchoring, and rule compilation. Run once per
    /// load, after every source has been overlaid.
    void finalize(CanonicalRef workspace_root);

    /// After finalize: move off a cache directory another project owns
    /// (owned_elsewhere) to the default one under the workspace root.
    void keep_own_cache_dir();

    /// The compiled rules applying to `path`, in declaration order,
    /// restricted to untagged rules and rules tagged `configuration`.
    llvm::SmallVector<const CompiledRule*> matching_rules(CanonicalRef path,
                                                          llvm::StringRef configuration) const;

    /// The distinct configuration tags, in first-appearance order.
    llvm::SmallVector<llvm::StringRef> configurations() const;

    /// Try to load configuration from a TOML file. Its relative paths and
    /// patterns anchor at the file's directory: every rule records it, so a
    /// source overlaid later keeps its own anchor. Parse/validation problems are appended to
    /// `issues` when provided: decode failures as Error (the caller falls
    /// back to defaults), unknown keys as Warning (the rest of the file
    /// still applies). Set `finalized` to false when further config sources
    /// will be overlaid before finalize() runs — derived fields (cache_dir,
    /// logging_dir, ...) must be computed only once, from the final merged
    /// values.
    static std::optional<Config> load(llvm::StringRef path,
                                      CanonicalRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      bool finalized = true);

    /// Try to load configuration from a JSON string (e.g. initializationOptions).
    static std::optional<Config> load_from_json(llvm::StringRef json, CanonicalRef workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config if no file is found. `loaded_path`, when
    /// provided, receives the path of the config file that was found (even
    /// if it failed to parse), or stays empty. `finalized` as in load().
    static Config load_from_workspace(CanonicalRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      std::string* loaded_path = nullptr,
                                      bool finalized = true);

    /// The configuration's JSON schema (draft 2020-12), pretty-printed.
    /// Fields whose defaults derive from the running machine (the worker
    /// counts follow the CPU count) carry no `default` annotation, so the
    /// schema is byte-identical on every host. Unknown properties are
    /// rejected — the schema-side face of load()'s unknown-key warnings.
    static std::expected<std::string, std::string> json_schema();
};

}  // namespace clice
