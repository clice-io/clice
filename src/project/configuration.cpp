module;

#include "support/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :config.config;
import :index.database;
import :project.configuration;
import :support.anomaly;
import :support.logging;
import :vfs.path;

namespace clice {

namespace {

/// The workspace state that outlives a server process and the cache
/// layout version alike: kept at the cache directory root, next to the
/// ignore markers, not inside the versioned store.
struct PersistedState {
    std::string configuration;
};

std::string state_path(llvm::StringRef cache_dir) {
    return path::join(cache_dir, "state.json");
}

/// The real paths of the databases the rules active under `configuration`
/// declare, in priority order.
std::vector<std::string> real_databases(const Config& config, llvm::StringRef configuration) {
    std::vector<std::string> paths;
    for(auto& database: config.declared_databases(configuration)) {
        paths.push_back(CanonicalPath(database).str());
    }
    return paths;
}

/// What the rules active under `configuration` do besides naming
/// databases, one string each in declaration order: configurations alike in
/// it and in the databases they read build alike.
std::vector<std::string> edits_of(const Config& config, llvm::StringRef configuration) {
    std::vector<std::string> edits;
    for(auto rule: config.rules) {
        if(!rule.configuration.empty() && rule.configuration != configuration) {
            continue;
        }
        rule.configuration.clear();
        rule.compile_commands.clear();
        rule.resolve_symlinks = false;
        edits.push_back(json::to_string(rule).value_or(""));
    }
    return edits;
}

}  // namespace

llvm::StringRef fallback_configuration(const Config& config) {
    auto tags = config.configurations();
    if(tags.empty()) {
        return {};
    }
    llvm::StringRef preferred = config.default_configuration;
    return ranges::contains(tags, preferred) ? preferred : tags.front();
}

std::string read_selection(llvm::StringRef cache_dir) {
    if(cache_dir.empty()) {
        return {};
    }
    auto path = state_path(cache_dir);
    std::string content;
    if(auto error = fs::read_file(content, path)) {
        if(error != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read {}: {}", path, error.message());
        }
        return {};
    }
    PersistedState state;
    if(auto parsed = json::from_string(content, state); !parsed) {
        LOG_WARN("Ignoring malformed {}: {}", path, parsed.error().message);
        return {};
    }
    return std::move(state.configuration);
}

std::expected<void, std::error_code> write_selection(llvm::StringRef cache_dir,
                                                     llvm::StringRef configuration) {
    if(cache_dir.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if(auto made = fs::create_directories(cache_dir); !made) {
        return std::unexpected(made.error());
    }
    auto json = json::to_string(PersistedState{.configuration = configuration.str()});
    if(!json) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if(auto ec = fs::write_file_atomic(state_path(cache_dir), *json + '\n')) {
        return std::unexpected(ec);
    }
    return {};
}

bool declares_configuration(const Config& config, llvm::StringRef name) {
    return ranges::contains(config.configurations(), name);
}

std::expected<void, std::string> check_requested_configuration(const Config& config,
                                                               llvm::StringRef requested) {
    if(requested.empty() || declares_configuration(config, requested)) {
        return {};
    }
    auto tags = config.configurations();
    if(tags.empty()) {
        return std::unexpected(
            std::format("--configuration {} names no configuration: no rule declares one",
                        requested));
    }
    return std::unexpected(std::format("--configuration {} names no rule's configuration ({})",
                                       requested,
                                       llvm::join(tags, ", ")));
}

std::string resolve_configuration(const Config& config, llvm::StringRef requested) {
    auto tags = config.configurations();
    if(tags.empty()) {
        return {};
    }
    auto fallback = fallback_configuration(config);
    if(!requested.empty()) {
        if(ranges::contains(tags, requested)) {
            LOG_INFO("Active configuration: {} (--configuration)", requested);
            return requested.str();
        }
        LOG_GUIDANCE("--configuration {} names no rule's configuration ({}); ignoring it",
                     requested,
                     llvm::join(tags, ", "));
    }
    if(auto selected = read_selection(config.project.cache_dir); !selected.empty()) {
        if(ranges::contains(tags, llvm::StringRef(selected))) {
            LOG_INFO("Active configuration: {} (selected)", selected);
            return selected;
        }
        LOG_GUIDANCE(
            "The selected configuration {} in {} names no rule's configuration ({}); "
            "using {}",
            selected,
            state_path(config.project.cache_dir),
            llvm::join(tags, ", "),
            fallback);
    }
    if(config.default_configuration.empty()) {
        LOG_WARN("Rules declare configurations {} but default_configuration is unset; using {}",
                 llvm::join(tags, ", "),
                 fallback);
    } else {
        LOG_INFO("Active configuration: {} (default)", fallback);
    }
    return fallback.str();
}

std::string resolved_databases(const Config& config, llvm::StringRef configuration) {
    std::string resolved;
    for(auto& rule: config.compiled_rules) {
        if(!rule.resolve_symlinks || !rule.active_under(configuration)) {
            continue;
        }
        for(auto& database: rule.compile_commands) {
            if(!resolved.empty()) {
                resolved += '\n';
            }
            resolved += CanonicalPath(database).str();
        }
    }
    return resolved;
}

index::DatabaseKey database_key(const Config& config,
                                llvm::StringRef configuration,
                                llvm::StringRef resolved) {
    if(resolved.empty()) {
        return {.configuration = configuration.str()};
    }
    auto databases = real_databases(config, configuration);
    auto edits = edits_of(config, configuration);
    for(auto tag: config.configurations()) {
        auto resolves = [&](const CompiledRule& rule) {
            return rule.resolve_symlinks && rule.active_under(tag);
        };
        if(tag != configuration && ranges::none_of(config.compiled_rules, resolves) &&
           real_databases(config, tag) == databases && edits_of(config, tag) == edits) {
            return {.configuration = tag.str()};
        }
    }
    return {.configuration = configuration.str(), .resolved = resolved.str()};
}

}  // namespace clice
