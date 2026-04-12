#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kota/meta/annotation.h"
#include "support/glob_pattern.h"

#include "llvm/ADT/StringRef.h"

namespace clice {

using kota::meta::annotation;
using kota::meta::defaulted;
namespace serde_schema = kota::meta::attrs;

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
    defaulted<std::string> index_dir;
    defaulted<std::string> logging_dir;

    defaulted<std::vector<std::string>> compile_commands_paths;

    std::optional<bool> enable_indexing;
    std::optional<int> idle_timeout_ms;

    defaulted<std::uint32_t> stateful_worker_count = {};
    defaulted<std::uint32_t> stateless_worker_count = {};
    defaulted<std::uint64_t> worker_memory_limit = {};
};

struct CompiledRule {
    std::vector<GlobPattern> patterns;
    std::vector<std::string> append;
    std::vector<std::string> remove;
};

/// Configuration for the clice LSP server, loadable from clice.toml
/// or passed via LSP initializationOptions.
struct CliceConfig {
    defaulted<ProjectConfig> project;

    defaulted<std::vector<ConfigRule>> rules;

    annotation<std::vector<CompiledRule>, serde_schema::skip> compiled_rules;

    /// Compute default values for any field left at its zero/empty sentinel.
    void apply_defaults(const std::string& workspace_root);

    /// Collect append/remove flags from all rules whose patterns match `path`.
    void match_rules(llvm::StringRef path,
                     std::vector<std::string>& append,
                     std::vector<std::string>& remove) const;

    /// Try to load configuration from a TOML file.
    static std::optional<CliceConfig> load(const std::string& path,
                                           const std::string& workspace_root);

    /// Try to load configuration from a JSON string (e.g. initializationOptions).
    static std::optional<CliceConfig> load_from_json(llvm::StringRef json,
                                                     const std::string& workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config (with apply_defaults) if no file is found.
    static CliceConfig load_from_workspace(const std::string& workspace_root);
};

}  // namespace clice
