module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :project.command_snapshot;
import :project.configuration;
import :project.load;
import :project.open_index;
import :support.logging;
import :vfs.cache_store;
import :vfs.path;

namespace clice {

std::string damaged_index_error(const Project& project) {
    return std::format(
        "the index database at {} is damaged (the log above says where); delete that "
        "directory and run `clice index` to rebuild it",
        index::database_directory(*project.store, project.build.active_configuration()));
}

Spelling inspected_path(const Project& project, llvm::StringRef argument) {
    return Spelling(argument, Spelling(project.config.workspace_root));
}

namespace {

/// Resolve the configuration and open the store and database read-only.
bool open_database(Project& project, Config config, llvm::StringRef requested_configuration) {
    auto configuration = resolve_configuration(config, requested_configuration);
    // Read-only: the default cache directory exists as soon as the config
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a stats reader.
    auto store =
        CacheStore::open(config.project.cache_dir, cache_format_version, /*read_only=*/true);
    if(!store) {
        if(store.error() == std::errc::no_such_file_or_directory) {
            LOG_ERROR("No index cache at {}; run `clice index` first",
                      std::string_view(config.project.cache_dir));
        } else {
            LOG_ERROR("Failed to open cache store at {}: {}",
                      std::string_view(config.project.cache_dir),
                      store.error().message());
        }
        return false;
    }
    project.config = std::move(config);
    project.index.workspace = project.config.workspace_root;
    project.store.emplace(std::move(*store));
    project.build.reset_active(configuration);
    project.database = index::open_database(*project.store, configuration, /*read_only=*/true);
    if(!project.database) {
        // A database that exists but did not open said why as it failed.
        if(!index::database_exists(*project.store, configuration)) {
            LOG_ERROR("No index cache at {}; run `clice index` first",
                      index::database_directory(*project.store, configuration));
        }
        return false;
    }
    return true;
}

void log_unusable_format(const Project& project) {
    LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
              std::string_view(project.config.project.cache_dir));
}

}  // namespace

bool open_index(Project& project, Config config, llvm::StringRef requested_configuration) {
    if(!open_database(project, std::move(config), requested_configuration)) {
        return false;
    }
    switch(project.index.open(*project.database)) {
        case index::LoadOutcome::Loaded: return true;
        case index::LoadOutcome::Empty:
            LOG_ERROR("No index cache at {}; run `clice index` first",
                      std::string_view(project.config.project.cache_dir));
            return false;
        case index::LoadOutcome::Undecodable: log_unusable_format(project); return false;
        case index::LoadOutcome::Unreadable:
            LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
                      std::string_view(project.config.project.cache_dir));
            return false;
    }
    std::unreachable();
}

std::optional<LoadedIndex> load_index(Project& project,
                                      CommandResolver& commands,
                                      Config config,
                                      llvm::StringRef requested_configuration,
                                      bool with_build) {
    if(!open_database(project, std::move(config), requested_configuration)) {
        return std::nullopt;
    }
    // The writer's load, read-only: it never saves on a read-only database.
    auto loaded = load_saved(project, commands);
    if(!loaded.decoded) {
        log_unusable_format(project);
        return std::nullopt;
    }
    // load_saved detaches the database when the header exists but cannot
    // be read — a transient IO error, not an empty index.
    if(project.database == nullptr) {
        LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
                  std::string_view(project.config.project.cache_dir));
        return std::nullopt;
    }
    LoadedIndex result;
    result.contexts = std::move(project.contexts.bytes);
    if(with_build) {
        auto remembered = CommandSnapshot::remembered(project);
        load_build(project,
                   project.config.workspace_root,
                   project.build.active_configuration(),
                   remembered.sources,
                   remembered.provisional);
    }
    return result;
}

}  // namespace clice
