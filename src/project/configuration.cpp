module;

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"
#include "support/prelude.h"

module clice;

import :config.config;
import :project.configuration;
import :support.anomaly;
import :support.logging;
import :vfs.file_system;
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
    auto content = vfs::read(path, vfs::Read::Bytes);
    if(!content) {
        if(content.error() != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read {}: {}", path, content.error().message());
        }
        return {};
    }
    PersistedState state;
    if(auto parsed = json::from_string((*content)->getBuffer(), state); !parsed) {
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
    if(auto ec = vfs::create_directories(cache_dir)) {
        return std::unexpected(ec);
    }
    auto json = json::to_string(PersistedState{.configuration = configuration.str()});
    if(!json) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    if(auto ec = vfs::write_atomic(state_path(cache_dir), *json + '\n')) {
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

}  // namespace clice
