module clice;

import :settings.settings;
import :settings.configuration;
import :support.anomaly;
import :support.logging;
import :vfs.path;

namespace clice {

namespace {

/// The project state that outlives a server process and the cache
/// layout version alike: kept at the cache directory root, next to the
/// ignore markers, not inside the versioned store.
struct PersistedState {
    std::string configuration;
};

std::string state_path(llvm::StringRef cache_dir) {
    return path::join(cache_dir, "state.json");
}

}  // namespace

std::string read_selected_configuration(llvm::StringRef cache_dir) {
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

std::expected<void, std::error_code> write_selected_configuration(llvm::StringRef cache_dir,
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

std::string choose_configuration(const Settings& settings,
                                 llvm::StringRef requested,
                                 bool strict,
                                 std::vector<SettingsIssue>& issues) {
    auto tags = settings.configurations();
    if(!requested.empty() && !settings.declares(requested)) {
        issues.push_back({
            .severity = strict ? SettingsIssue::Severity::Error : SettingsIssue::Severity::Warning,
            .message = tags.empty()
                           ? std::format("--configuration {} names no configuration: no rule "
                                         "declares one",
                                         requested)
                           : std::format("--configuration {} names no rule's configuration ({})",
                                         requested,
                                         llvm::join(tags, ", ")),
        });
        requested = {};
    }
    if(tags.empty()) {
        return {};
    }
    if(!requested.empty()) {
        LOG_INFO("Active configuration: {} (--configuration)", requested);
        return requested.str();
    }
    auto fallback = settings.fallback_configuration();
    if(auto selected = read_selected_configuration(settings.project.cache_dir); !selected.empty()) {
        if(settings.declares(selected)) {
            LOG_INFO("Active configuration: {} (selected)", selected);
            return selected;
        }
        issues.push_back({
            .severity = SettingsIssue::Severity::Warning,
            .message = std::format(
                "The selected configuration {} in {} names no rule's configuration ({}); using {}",
                selected,
                state_path(settings.project.cache_dir),
                llvm::join(tags, ", "),
                fallback),
        });
    }
    if(settings.default_configuration.empty()) {
        LOG_WARN("Rules declare configurations {} but default_configuration is unset; using {}",
                 llvm::join(tags, ", "),
                 fallback);
    } else {
        LOG_INFO("Active configuration: {} (default)", fallback);
    }
    return fallback.str();
}

}  // namespace clice
