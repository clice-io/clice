#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "feature/feature.h"
#include "support/glob_pattern.h"

#include "kota/codec/macro.h"
#include "kota/meta/annotation.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// Defaults that are computed rather than written: a fresh Config queries
/// them in its field initializers, so a default-constructed Config is
/// already fully valid ("born valid") and no later pass fills options in.
std::uint32_t default_stateless_worker_count();
std::uint32_t default_max_stateless_worker_count();

/// A file-pattern rule that appends/removes compilation flags.
/// Corresponds to `[[rules]]` in clice.toml.
struct ConfigRule {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Glob patterns selecting the files this rule applies to.")
    <std::vector<std::string>> patterns;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Compilation flags appended for matching files.")
    <std::vector<std::string>> append;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Compilation flags removed for matching files.")
    <std::vector<std::string>> remove;
};

/// Corresponds to the `[project]` section in clice.toml. Field
/// initializers are the defaults; `cache_dir` and `logging_dir` stay empty
/// here because their defaults derive from the workspace root in
/// finalize().
struct ProjectConfig {
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Run clang-tidy alongside compiler diagnostics.")
    <bool> clang_tidy = false;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Directory for the index and PCH cache; empty derives it "
                         "from the workspace root.")
    <std::string> cache_dir;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Directory for log files; empty derives it from the "
                         "cache directory.")
    <std::string> logging_dir;

    KOTATSU_ANNOTATE(defaulted = true, description = "Paths searched for compile_commands.json.")
    <std::vector<std::string>> compile_commands_paths;

    KOTATSU_ANNOTATE(defaulted = true, description = "Build the background index.")
    <bool> enable_indexing = true;

    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Idle delay in milliseconds before background indexing "
                         "starts.")
    <int> idle_timeout_ms = 3000;

    /// The hooks can generate load on demand (log floods).
    KOTATSU_ANNOTATE(defaulted = true,
                     description =
                         "Enable the clice/internal test hooks used by the test "
                         "harness.")
    <bool> test_hooks = false;

    KOTATSU_ANNOTATE(defaulted = true, description = "Number of stateful workers.")
    <std::uint32_t> stateful_worker_count = 2;

    KOTATSU_ANNOTATE(defaulted = true, description = "Initial number of stateless workers.")
    <std::uint32_t> stateless_worker_count = default_stateless_worker_count();

    /// See WorkerPoolOptions.
    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Lower bound for dynamic stateless-worker scaling.")
    <std::uint32_t> min_stateless_worker_count = 1;

    KOTATSU_ANNOTATE(defaulted = true,
                     description = "Upper bound for dynamic stateless-worker scaling.")
    <std::uint32_t> max_stateless_worker_count = default_max_stateless_worker_count();

    KOTATSU_ANNOTATE(defaulted = true, description = "Per-stateful-worker memory limit in bytes.")
    <std::uint64_t> worker_memory_limit = 4ULL * 1024 * 1024 * 1024;
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

struct CompiledRule {
    std::vector<GlobPattern> patterns;
    std::vector<std::string> append;
    std::vector<std::string> remove;
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

    /// Compute the values derived from the final merged config: default
    /// cache/logging directories, ${workspace} substitution, path
    /// canonicalization, and rule glob compilation. Run once per load,
    /// after every source has been overlaid.
    void finalize(llvm::StringRef workspace_root);

    /// Collect append/remove flags from all rules whose patterns match `path`.
    void match_rules(llvm::StringRef path,
                     std::vector<std::string>& append,
                     std::vector<std::string>& remove) const;

    /// Try to load configuration from a TOML file. Parse/validation problems
    /// are appended to `issues` when provided: decode failures as Error (the
    /// caller falls back to defaults), unknown keys as Warning (the rest of
    /// the file still applies). Set `finalized` to false when further config
    /// sources will be overlaid before finalize() runs — derived fields
    /// (cache_dir, logging_dir, ...) must be computed only once, from the
    /// final merged values.
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
};

}  // namespace clice
