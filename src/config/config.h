#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "feature/feature.h"

#include "kota/codec/macro.h"
#include "kota/meta/annotation.h"
#include "kota/support/glob_pattern.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Defaults that are computed rather than written: a fresh Config queries
/// them in its field initializers, so a default-constructed Config is
/// already fully valid ("born valid") and no later pass fills options in.
std::uint32_t default_stateless_worker_count();
std::uint32_t default_max_stateless_worker_count();

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
                         "configuration menu, and `default_configuration` "
                         "names the one active at startup.")
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
                         "declares a source, the workspace root and its "
                         "immediate subdirectories are searched for one.")
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
                         "join the background index — enumerated at startup, "
                         "so a file created later compiles when opened and "
                         "joins the index at the next start. Omitted means "
                         "none.")
    <CommandSpelling> default_command;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation flags appended for matching files, e.g. "
                         "`[\"-std=c++20\", \"-DNDEBUG\"]`.")
    <std::vector<std::string>> append;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation flags removed for matching files, e.g. "
                         "`[\"-Wall\"]`.")
    <std::vector<std::string>> remove;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Whether matching translation units join the "
                         "background index. `false` keeps them out; they still "
                         "compile when opened and still host the headers they "
                         "include. Any matching rule saying `false` wins.")
    <bool> index = true;

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
                     description =
                         "Number of stateful workers — they hold ASTs in memory "
                         "and serve queries (hover, semantic tokens, ...); `0` is "
                         "invalid and falls back to the default.")
    <std::uint32_t> stateful_worker_count = 2;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Initial number of stateless workers — they handle "
                         "ephemeral tasks (PCH/PCM builds, completion, signature "
                         "help); defaults to half the machine's parallelism, at "
                         "least 2. `0` is invalid and falls back to that default.")
    <std::uint32_t> stateless_worker_count = default_stateless_worker_count();

    /// See WorkerPoolOptions.
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Lower bound for dynamic stateless-worker scaling; `0` "
                         "is invalid and falls back to the default.")
    <std::uint32_t> min_stateless_worker_count = 1;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Upper bound for dynamic stateless-worker scaling; `0` "
                         "means the machine's parallelism, which is also the "
                         "default.")
    <std::uint32_t> max_stateless_worker_count = default_max_stateless_worker_count();
};

/// Corresponds to the `[tracker]` section in clice.toml: the stat-polling
/// file tracker's intervals (integration tests drive ticks through the
/// clice/internal/poll hook instead).
struct TrackerConfig {
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Compilation database poll interval in seconds; 0 disables "
                         "polling.")
    <std::uint32_t> cdb_poll_seconds = 3;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Workspace file sweep interval in seconds; 0 disables "
                         "polling.")
    <std::uint32_t> workspace_poll_seconds = 30;
};

/// A rule after finalize(): patterns compiled, paths anchored.
struct CompiledRule {
    struct Pattern {
        /// Matches the canonical absolute path: a relative pattern was
        /// anchored at the configuration file's directory when compiled.
        kota::GlobPattern glob;

        /// The literal directory the pattern starts in (the workspace root
        /// for `**`-led patterns): where the files it claims are enumerated.
        std::string root;
    };

    std::vector<Pattern> patterns;
    std::string configuration;
    /// Absolute paths of the declared databases, in priority order; an
    /// existing directory resolved to the compile_commands.json under it.
    std::vector<std::string> compile_commands;
    /// The command's argv (a string spelling tokenized with the host's
    /// shell rules), `${workspace}` substituted; empty means none.
    /// `directory` is its working directory.
    std::vector<std::string> default_command;
    std::string directory;
    std::vector<std::string> append;
    std::vector<std::string> remove;
    bool index = true;

    /// Every pattern failed to compile: the rule matches no file, but the
    /// sources it declares still load.
    bool unmatchable = false;

    bool has_default_command() const;

    /// Whether the rule declares a command source — databases, or a default
    /// command the rule can actually hand out — rather than only editing
    /// commands.
    bool declares_sources() const;

    /// Whether the rule applies to `path` (canonical absolute).
    bool matches(llvm::StringRef path) const;
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
                         "The build configuration active at startup, one of the "
                         "tags declared on rules. When rules carry tags and this "
                         "names none of them, the first declared tag is used and "
                         "a warning is logged.")
    <std::string> default_configuration;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [project] section: project-wide server options.")
    <ProjectConfig> project;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [tracker] section: file tracker poll intervals.")
    <TrackerConfig> tracker;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [hover] section: hover rendering options.")
    <feature::HoverOptions> hover;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [inlay_hints] section: inlay hint options.")
    <feature::InlayHintsOptions> inlay_hints;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "The [code_completion] section: code completion options.")
    <feature::CodeCompletionOptions> code_completion;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "File-pattern rules that adjust compilation flags "
                         "([[rules]] in clice.toml).")
    <std::vector<ConfigRule>> rules;

    KOTATSU_ANNOTATE(skip = true)
    <std::vector<CompiledRule>> compiled_rules;

    /// The workspace root finalize() ran for, canonical: the `${workspace}`
    /// value, the anchor of rules and databases no configuration file
    /// supplied, and the enumeration root of `**`-led patterns.
    KOTATSU_ANNOTATE(skip = true)
    <std::string> workspace_root;

    /// Compute the values derived from the final merged config: default
    /// cache/logging directories, ${workspace} substitution, path
    /// canonicalization and anchoring, and rule compilation. Run once per
    /// load, after every source has been overlaid.
    void finalize(llvm::StringRef workspace_root);

    /// The compiled rules applying to `path` (absolute), in declaration
    /// order, restricted to untagged rules and rules tagged
    /// `configuration`.
    llvm::SmallVector<const CompiledRule*> matching_rules(llvm::StringRef path,
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
                                      llvm::StringRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      bool finalized = true);

    /// Try to load configuration from a JSON string (e.g. initializationOptions).
    static std::optional<Config> load_from_json(llvm::StringRef json,
                                                llvm::StringRef workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config if no file is found. `loaded_path`, when
    /// provided, receives the path of the config file that was found (even
    /// if it failed to parse), or stays empty. `finalized` as in load().
    static Config load_from_workspace(llvm::StringRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      std::string* loaded_path = nullptr,
                                      bool finalized = true);

    /// The configuration's JSON schema (draft 2020-12), pretty-printed.
    /// Fields whose defaults derive from the running machine (the worker
    /// counts follow the CPU count) carry no `default` annotation, so the
    /// schema is byte-identical on every host. Unknown properties are
    /// rejected — the schema-side face of the strict decode pass's typo
    /// warnings.
    static std::expected<std::string, std::string> json_schema();
};

}  // namespace clice
