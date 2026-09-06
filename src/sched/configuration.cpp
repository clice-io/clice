#include "sched/configuration.h"

#include "config/config.h"
#include "support/anomaly.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/codec/json/json.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"

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
    return llvm::is_contained(tags, preferred) ? preferred : tags.front();
}

std::string read_selection(llvm::StringRef cache_dir) {
    if(cache_dir.empty()) {
        return {};
    }
    auto path = state_path(cache_dir);
    auto content = fs::read(path);
    if(!content) {
        if(content.error() != std::errc::no_such_file_or_directory) {
            LOG_WARN("Cannot read {}: {}", path, content.error().message());
        }
        return {};
    }
    PersistedState state;
    if(auto parsed = kota::codec::json::from_string(*content, state); !parsed) {
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
    if(auto ec = llvm::sys::fs::create_directories(cache_dir)) {
        return std::unexpected(ec);
    }
    auto json = kota::codec::json::to_string(PersistedState{.configuration = configuration.str()});
    if(!json) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    auto path = state_path(cache_dir);
    llvm::SmallString<256> tmp_path;
    if(auto ec = llvm::sys::fs::createUniqueFile(path + ".%%%%%%", tmp_path)) {
        return std::unexpected(ec);
    }
    auto written = fs::write(tmp_path, *json + '\n');
    if(written) {
        written = fs::rename(tmp_path, path);
    }
    if(!written) {
        llvm::sys::fs::remove(tmp_path);
    }
    return written;
}

bool declares_configuration(const Config& config, llvm::StringRef name) {
    return llvm::is_contained(config.configurations(), name);
}

bool check_requested_configuration(const Config& config, llvm::StringRef requested) {
    if(requested.empty() || declares_configuration(config, requested)) {
        return true;
    }
    LOG_ERROR("--configuration {} names no rule's configuration ({})",
              requested,
              llvm::join(config.configurations(), ", "));
    return false;
}

std::string resolve_configuration(const Config& config, llvm::StringRef requested) {
    auto tags = config.configurations();
    if(tags.empty()) {
        return {};
    }
    auto fallback = fallback_configuration(config);
    if(!requested.empty()) {
        if(llvm::is_contained(tags, requested)) {
            LOG_INFO("Active configuration: {} (--configuration)", requested);
            return requested.str();
        }
        LOG_GUIDANCE("--configuration {} names no rule's configuration ({}); ignoring it",
                     requested,
                     llvm::join(tags, ", "));
    }
    if(auto selected = read_selection(config.project.cache_dir); !selected.empty()) {
        if(llvm::is_contained(tags, llvm::StringRef(selected))) {
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
    LOG_INFO("Active configuration: {} (default)", fallback);
    return fallback.str();
}

}  // namespace clice
