#include "project/open_index.h"

#include "index/database.h"
#include "project/configuration.h"
#include "project/index_store.h"
#include "project/load.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/async.h"

namespace clice {

std::string inspected_path(const Project& project, llvm::StringRef argument) {
    llvm::SmallString<256> absolute(path::is_absolute(argument)
                                        ? argument.str()
                                        : path::join(project.config.workspace_root, argument));
    path::remove_dots(absolute, /*remove_dot_dot=*/true);
    std::string result(absolute.str());
    path::canonicalize(result);
    return result;
}

namespace {

/// Resolve the configuration and open the store and database read-only.
bool open_database(Project& project,
                   llvm::StringRef root,
                   llvm::StringRef requested_configuration) {
    auto config = Config::load_from_workspace(root);
    if(!check_requested_configuration(config, requested_configuration)) {
        return false;
    }
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
    project.store.emplace(std::move(*store));
    project.build.reset_active(configuration);
    project.index_db = index::open_database(*project.store, configuration, /*read_only=*/true);
    if(!project.index_db) {
        LOG_ERROR("No index cache at {}; run `clice index` first",
                  index::library_directory(*project.store, configuration));
        return false;
    }
    return true;
}

}  // namespace

bool open_index(Project& project, llvm::StringRef root, llvm::StringRef requested_configuration) {
    if(!open_database(project, root, requested_configuration)) {
        return false;
    }
    if(!project.project_index.open(*project.index_db, project.file_table)) {
        LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                  std::string_view(project.config.project.cache_dir));
        return false;
    }
    return true;
}

std::optional<LoadedIndex> load_index(Project& project,
                                      CommandResolver& commands,
                                      ContextsOwner* contexts,
                                      llvm::StringRef root,
                                      llvm::StringRef requested_configuration,
                                      bool with_build) {
    if(!open_database(project, root, requested_configuration)) {
        return std::nullopt;
    }
    // The store is the writer's engine; its load is the only reader of
    // manifests, and never saves on a read-only database.
    kota::event_loop loop;
    IndexStore store{loop, project, commands};
    if(contexts) {
        store.attach_contexts(*contexts);
    }
    auto loaded = store.load({.read_only = true, .borrow = true});
    if(!loaded.decoded) {
        LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                  std::string_view(project.config.project.cache_dir));
        return std::nullopt;
    }
    // load() detaches the storage when the global blob exists but cannot
    // be read — a transient IO error, not an empty index.
    if(project.index_db == nullptr) {
        LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
                  std::string_view(project.config.project.cache_dir));
        return std::nullopt;
    }
    LoadedIndex result;
    result.dropped.assign(loaded.report.reindex().begin(), loaded.report.reindex().end());
    // With no pump attached the load report's debt can only be the
    // recovery drops: every TU's blobs were missing, stale, or corrupt — a
    // damaged cache, not a legitimately empty one.
    if(project.project_index.manifests.empty() && project.project_index.shards.empty() &&
       !result.dropped.empty()) {
        LOG_ERROR(
            "Index cache at {} has no servable data ({} translation units need "
            "reindexing); run `clice index` to rebuild",
            std::string_view(project.config.project.cache_dir),
            result.dropped.size());
        return std::nullopt;
    }
    if(with_build) {
        load_build(project, root, project.build.active_configuration(), store.remembered_sources());
    }
    return result;
}

}  // namespace clice
