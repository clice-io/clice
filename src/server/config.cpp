#include "server/config.h"

#include <algorithm>
#include <thread>

#include "eventide/serde/toml.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice {

/// Replace all occurrences of ${workspace} with the workspace root.
static void substitute_workspace(std::string& value, const std::string& workspace_root) {
    constexpr std::string_view placeholder = "${workspace}";
    std::string::size_type pos = 0;
    while((pos = value.find(placeholder, pos)) != std::string::npos) {
        value.replace(pos, placeholder.size(), workspace_root);
        pos += workspace_root.size();
    }
}

void CliceConfig::apply_defaults(const std::string& workspace_root) {
    auto cpu_count = std::thread::hardware_concurrency();
    if(cpu_count == 0)
        cpu_count = 4;

    if(stateful_worker_count == 0) {
        stateful_worker_count = std::max(1u, cpu_count / 4);
    }
    if(stateless_worker_count == 0) {
        stateless_worker_count = std::max(1u, cpu_count / 4);
    }
    if(worker_memory_limit == 0) {
        worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB default
    }
    if(cache_dir.empty() && !workspace_root.empty()) {
        cache_dir = path::join(workspace_root, ".clice");
    }

    if(index_dir.empty() && !cache_dir.empty()) {
        index_dir = path::join(cache_dir, "index");
    }

    // Apply variable substitution to string fields
    substitute_workspace(compile_commands_path, workspace_root);
    substitute_workspace(cache_dir, workspace_root);
    substitute_workspace(index_dir, workspace_root);
}

std::optional<CliceConfig> CliceConfig::load(const std::string& path,
                                             const std::string& workspace_root) {
    auto content = fs::read(path);
    if(!content) {
        return std::nullopt;
    }

    auto result = eventide::serde::toml::parse<CliceConfig>(*content);
    if(!result) {
        LOG_WARN("Failed to parse config file {}", path);
        return std::nullopt;
    }

    auto config = std::move(*result);
    config.apply_defaults(workspace_root);

    LOG_INFO("Loaded config from {}", path);
    return config;
}

CliceConfig CliceConfig::load_from_workspace(const std::string& workspace_root) {
    if(!workspace_root.empty()) {
        // Try standard config file locations
        for(auto* name: {"clice.toml", ".clice/config.toml"}) {
            auto config_path = path::join(workspace_root, name);
            if(llvm::sys::fs::exists(config_path)) {
                auto config = load(config_path, workspace_root);
                if(config)
                    return std::move(*config);
            }
        }
    }

    // No config file found; use defaults
    CliceConfig config;
    config.apply_defaults(workspace_root);
    LOG_INFO(
        "No clice.toml found, using default configuration " "(stateful={}, stateless={}, memory_limit={}MB)",
        config.stateful_worker_count,
        config.stateless_worker_count,
        config.worker_memory_limit / (1024 * 1024));
    return config;
}

}  // namespace clice
