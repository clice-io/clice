#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "support/glob_pattern.h"

#include "kota/meta/annotation.h"
#include "llvm/ADT/StringRef.h"

namespace clice {

/// A file-pattern rule that appends/removes compilation flags.
/// Corresponds to `[[rules]]` in clice.toml.
struct ConfigRule {
    kota::meta::defaulted<std::vector<std::string>> patterns;
    kota::meta::defaulted<std::vector<std::string>> append;
    kota::meta::defaulted<std::vector<std::string>> remove;
};

/// Corresponds to the `[project]` section in clice.toml.
struct ProjectConfig {
    kota::meta::defaulted<bool> clang_tidy = {};
    kota::meta::defaulted<int> max_active_file = {};

    kota::meta::defaulted<std::string> cache_dir;
    kota::meta::defaulted<std::string> index_dir;
    kota::meta::defaulted<std::string> logging_dir;

    kota::meta::defaulted<std::vector<std::string>> compile_commands_paths;

    std::optional<bool> enable_indexing;
    std::optional<int> idle_timeout_ms;

    kota::meta::defaulted<std::uint32_t> stateful_worker_count = {};
    kota::meta::defaulted<std::uint32_t> stateless_worker_count = {};
    kota::meta::defaulted<std::uint64_t> worker_memory_limit = {};
};

struct CompiledRule {
    std::vector<GlobPattern> patterns;
    std::vector<std::string> append;
    std::vector<std::string> remove;
};

/// Configuration for the clice LSP server, loadable from clice.toml
/// or passed via LSP initializationOptions.
struct Config {
    kota::meta::defaulted<ProjectConfig> project;

    kota::meta::defaulted<std::vector<ConfigRule>> rules;

    kota::meta::annotation<std::vector<CompiledRule>, kota::meta::attrs::skip> compiled_rules;

    /// Compute default values for any field left at its zero/empty sentinel.
    void apply_defaults(llvm::StringRef workspace_root);

    /// Collect append/remove flags from all rules whose patterns match `path`.
    void match_rules(llvm::StringRef path,
                     std::vector<std::string>& append,
                     std::vector<std::string>& remove) const;

    /// Try to load configuration from a TOML file.
    static std::optional<Config> load(llvm::StringRef path, llvm::StringRef workspace_root);

    /// Try to load configuration from a JSON string (e.g. initializationOptions).
    static std::optional<Config> load_from_json(llvm::StringRef json,
                                                llvm::StringRef workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config (with apply_defaults) if no file is found.
    static Config load_from_workspace(llvm::StringRef workspace_root);
};

}  // namespace clice
