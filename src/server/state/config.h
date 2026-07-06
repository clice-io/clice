#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support/glob_pattern.h"

#include "kota/meta/annotation.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

using kota::meta::defaulted;

/// A file-pattern rule that appends/removes compilation flags.
/// Corresponds to `[[rules]]` in clice.toml.
struct ConfigRule {
    defaulted<std::vector<std::string>> patterns;
    defaulted<std::vector<std::string>> append;
    defaulted<std::vector<std::string>> remove;
};

/// Corresponds to the `[project]` section in clice.toml.
struct ProjectConfig {
    defaulted<bool> clang_tidy = {};
    defaulted<int> max_active_file = {};

    defaulted<std::string> cache_dir;
    defaulted<std::string> logging_dir;

    defaulted<std::vector<std::string>> compile_commands_paths;

    std::optional<bool> enable_indexing;
    std::optional<int> idle_timeout_ms;

    defaulted<std::uint32_t> stateful_worker_count = {};
    defaulted<std::uint32_t> stateless_worker_count = {};
    defaulted<std::uint32_t> min_stateless_worker_count = {};
    defaulted<std::uint32_t> max_stateless_worker_count = {};
    defaulted<std::uint64_t> worker_memory_limit = {};
};

/// Corresponds to the `[tracker]` section in clice.toml: the stat-polling
/// file tracker's intervals. 0 disables the loop (integration tests drive
/// ticks through the clice/internal/poll hook instead).
struct TrackerConfig {
    /// Compilation database poll interval in seconds (default 3).
    std::optional<std::uint32_t> cdb_poll_seconds;
    /// Workspace file sweep interval in seconds (default 30).
    std::optional<std::uint32_t> workspace_poll_seconds;
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
struct Config {
    defaulted<ProjectConfig> project;

    defaulted<TrackerConfig> tracker;

    defaulted<std::vector<ConfigRule>> rules;

    kota::meta::annotation<std::vector<CompiledRule>, kota::meta::attrs::skip> compiled_rules;

    /// Compute default values for any field left at its zero/empty sentinel.
    void apply_defaults(llvm::StringRef workspace_root);

    /// Collect append/remove flags from all rules whose patterns match `path`.
    void match_rules(llvm::StringRef path,
                     std::vector<std::string>& append,
                     std::vector<std::string>& remove) const;

    /// Try to load configuration from a TOML file. Parse/validation problems
    /// are appended to `issues` when provided: decode failures as Error (the
    /// caller falls back to defaults), unknown keys as Warning (the rest of
    /// the file still applies). Set `with_defaults` to false when further
    /// config sources will be overlaid before apply_defaults() runs — derived
    /// fields (index_dir, logging_dir, ...) must be computed only once, from
    /// the final merged values.
    static std::optional<Config> load(llvm::StringRef path,
                                      llvm::StringRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      bool with_defaults = true);

    /// Try to load configuration from a JSON string (e.g. initializationOptions).
    static std::optional<Config> load_from_json(llvm::StringRef json,
                                                llvm::StringRef workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config (with apply_defaults) if no file is found.
    /// `loaded_path`, when provided, receives the path of the config file
    /// that was found (even if it failed to parse), or stays empty.
    /// `with_defaults` as in load().
    static Config load_from_workspace(llvm::StringRef workspace_root,
                                      std::vector<ConfigIssue>* issues = nullptr,
                                      std::string* loaded_path = nullptr,
                                      bool with_defaults = true);
};

}  // namespace clice
