#include "server/config.h"

#include <algorithm>

#include "support/filesystem.h"
#include "support/glob_pattern.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "kota/codec/toml.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/xxhash.h"

namespace clice {

/// Replace all occurrences of ${workspace} with the workspace root.
/// No-op when workspace_root is empty, to avoid producing paths like "/cache"
/// from "${workspace}/cache".
static void substitute_workspace(std::string& value, llvm::StringRef workspace_root) {
    if(workspace_root.empty())
        return;
    constexpr std::string_view placeholder = "${workspace}";
    std::size_t pos = 0;
    while((pos = value.find(placeholder, pos)) != std::string::npos) {
        value.replace(pos, placeholder.size(), workspace_root);
        pos += workspace_root.size();
    }
}

/// Try to resolve the default cache directory using XDG_CACHE_HOME.
/// Returns empty string on failure.
static std::string resolve_xdg_cache_dir(llvm::StringRef workspace_root) {
    // Determine base: $XDG_CACHE_HOME or ~/.cache
    std::string base;
    if(auto xdg = llvm::sys::Process::GetEnv("XDG_CACHE_HOME"); xdg && !xdg->empty()) {
        base = std::move(*xdg);
    } else if(auto home = llvm::sys::Process::GetEnv("HOME"); home && !home->empty()) {
        base = path::join(*home, ".cache");
    } else {
        return {};
    }

    // Use a hash of workspace_root to create a unique subdirectory.
    auto hash = llvm::xxh3_64bits(workspace_root);
    auto dir = path::join(base, "clice", std::format("{:016x}", hash));

    if(auto ec = llvm::sys::fs::create_directories(dir)) {
        LOG_WARN("Failed to create XDG cache directory {}: {}", dir, ec.message());
        return {};
    }
    return dir;
}

void CliceConfig::apply_defaults(llvm::StringRef workspace_root) {
    auto& p = project;

    if(p.max_active_file == 0)
        p.max_active_file = 8;
    if(!p.enable_indexing)
        p.enable_indexing = true;
    if(!p.idle_timeout_ms)
        p.idle_timeout_ms = 3000;

    if(p.stateful_worker_count == 0)
        p.stateful_worker_count = 2;
    if(p.stateless_worker_count == 0)
        p.stateless_worker_count = 3;
    if(p.worker_memory_limit == 0)
        p.worker_memory_limit = 4ULL * 1024 * 1024 * 1024;  // 4GB

    if(p.cache_dir.empty() && !workspace_root.empty()) {
        p.cache_dir = resolve_xdg_cache_dir(workspace_root);
        if(p.cache_dir.empty())
            p.cache_dir = path::join(workspace_root, ".clice");
    }
    if(p.index_dir.empty() && !p.cache_dir.empty())
        p.index_dir = path::join(p.cache_dir, "index");
    if(p.logging_dir.empty() && !p.cache_dir.empty())
        p.logging_dir = path::join(p.cache_dir, "logs");

    // Variable substitution on string fields.
    substitute_workspace(p.cache_dir, workspace_root);
    substitute_workspace(p.index_dir, workspace_root);
    substitute_workspace(p.logging_dir, workspace_root);
    for(auto& entry: p.compile_commands_paths)
        substitute_workspace(entry, workspace_root);

    // Pre-compile glob patterns from rules.
    compiled_rules.clear();
    for(auto& rule: rules) {
        CompiledRule compiled;
        for(auto& pattern_str: rule.patterns) {
            auto pat = GlobPattern::create(pattern_str);
            if(!pat) {
                LOG_WARN("Invalid glob pattern in rule: {}", pattern_str);
                continue;
            }
            compiled.patterns.push_back(std::move(*pat));
        }
        // Drop the whole rule if no pattern compiled successfully — otherwise the
        // append/remove flags would be silently attached to a rule that can never match.
        if(compiled.patterns.empty()) {
            if(!rule.patterns.empty())
                LOG_WARN("Rule dropped: all glob patterns failed to compile");
            continue;
        }
        compiled.append.assign(rule.append.begin(), rule.append.end());
        compiled.remove.assign(rule.remove.begin(), rule.remove.end());
        compiled_rules.push_back(std::move(compiled));
    }
}

void CliceConfig::match_rules(llvm::StringRef file_path,
                              std::vector<std::string>& append,
                              std::vector<std::string>& remove) const {
    for(auto& rule: compiled_rules) {
        bool matched =
            std::ranges::any_of(rule.patterns, [&](auto& pat) { return pat.match(file_path); });
        if(matched) {
            append.insert(append.end(), rule.append.begin(), rule.append.end());
            remove.insert(remove.end(), rule.remove.begin(), rule.remove.end());
        }
    }
}

std::optional<CliceConfig> CliceConfig::load(llvm::StringRef path, llvm::StringRef workspace_root) {
    auto content = fs::read(path);
    if(!content)
        return std::nullopt;

    auto result = kota::codec::toml::parse<CliceConfig>(*content);
    if(!result) {
        LOG_ERROR("Invalid clice.toml {}: {}", path, result.error().to_string());
        return std::nullopt;
    }

    auto config = std::move(*result);
    config.apply_defaults(workspace_root);
    LOG_INFO("Loaded config from {}", path);
    return config;
}

std::optional<CliceConfig> CliceConfig::load_from_json(llvm::StringRef json,
                                                       llvm::StringRef workspace_root) {
    auto result = kota::codec::json::from_json<CliceConfig>(json);
    if(!result) {
        LOG_WARN("Failed to parse initializationOptions JSON: {}", result.error().message());
        return std::nullopt;
    }

    auto config = std::move(*result);
    config.apply_defaults(workspace_root);
    LOG_INFO("Loaded config from initializationOptions");
    return config;
}

CliceConfig CliceConfig::load_from_workspace(llvm::StringRef workspace_root) {
    if(!workspace_root.empty()) {
        for(auto* name: {"clice.toml", ".clice/config.toml"}) {
            auto config_path = path::join(workspace_root, name);
            if(!llvm::sys::fs::exists(config_path))
                continue;
            if(auto config = load(config_path, workspace_root))
                return std::move(*config);
            // Present but malformed: fall through to defaults, but surface
            // the situation clearly so users know their config wasn't applied.
            LOG_WARN("Falling back to default configuration because {} is invalid", config_path);
        }
    }

    CliceConfig config;
    config.apply_defaults(workspace_root);
    LOG_INFO(
        "No clice.toml found, using default configuration " "(stateful={}, stateless={}, memory_limit={}MB)",
        config.project.stateful_worker_count.value,
        config.project.stateless_worker_count.value,
        config.project.worker_memory_limit.value / (1024 * 1024));
    return config;
}

}  // namespace clice
