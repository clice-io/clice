#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace clice {

/// Configuration for the clice LSP server, loadable from clice.toml.
struct CliceConfig {
    // Worker configuration (0 = auto-detect from system resources)
    std::uint32_t stateful_worker_count = 0;
    std::uint32_t stateless_worker_count = 0;
    std::uint64_t worker_memory_limit = 0;  // bytes; 0 = auto

    // Compilation database path (empty = auto-detect)
    std::string compile_commands_path;

    // Cache directory (empty = default: <workspace>/.clice/)
    std::string cache_dir;

    // Debounce interval for re-compilation after edits (milliseconds)
    int debounce_ms = 200;

    // Background indexing
    bool enable_indexing = true;
    int idle_timeout_ms = 3000;

    /// Compute default values for any field left at its zero/empty sentinel.
    void apply_defaults(const std::string& workspace_root);

    /// Try to load configuration from a TOML file.
    /// Performs ${workspace} variable substitution in string fields.
    /// Returns std::nullopt if the file does not exist or cannot be parsed.
    static std::optional<CliceConfig> load(const std::string& path,
                                            const std::string& workspace_root);

    /// Load config from the workspace, trying standard locations.
    /// Returns a default config (with apply_defaults) if no file is found.
    static CliceConfig load_from_workspace(const std::string& workspace_root);
};

}  // namespace clice
