#include "sched/index_view.h"

#include <chrono>
#include <thread>
#include <utility>

#include "index/database.h"
#include "sched/bootstrap.h"
#include "sched/configuration.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice {

std::string inspected_path(const IndexView& view, llvm::StringRef argument) {
    llvm::SmallString<256> absolute(
        path::is_absolute(argument) ? argument.str()
                                    : path::join(view.workspace.config.workspace_root, argument));
    path::remove_dots(absolute, /*remove_dot_dot=*/true);
    std::string result(absolute.str());
    path::canonicalize(result);
    return result;
}

namespace {

/// Sentinel of open_index: the load raced a live writer's batch; the caller
/// retries instead of reporting over the mid-write state.
constexpr int open_retry = -1;

/// Open the persisted index read-only into `view`. A non-zero return is
/// the command's exit code, the cause already logged; open_retry asks for
/// another attempt.
int open_index(IndexView& view,
               llvm::StringRef root,
               llvm::StringRef requested_configuration,
               bool allow_retry) {
    auto config = Config::load_from_workspace(root);
    if(!check_requested_configuration(config, requested_configuration)) {
        return 1;
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
        return 1;
    }

    auto& workspace = view.workspace;
    workspace.config = std::move(config);
    workspace.store.emplace(std::move(*store));
    workspace.build.reset_active(configuration);
    workspace.index_db = index::open_database(*workspace.store, configuration);
    if(!workspace.index_db) {
        LOG_ERROR("No index cache at {}; run `clice index` first",
                  index::library_directory(*workspace.store, configuration));
        return 1;
    }
    view.configuration = configuration;
    auto loaded = view.store.load({.read_only = true, .borrow = true});
    if(!loaded.decoded) {
        LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                  std::string_view(workspace.config.project.cache_dir));
        return 1;
    }
    // load() detaches the storage when the global blob exists but cannot
    // be read — a transient IO error, not an empty index.
    if(workspace.index_db == nullptr) {
        LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
                  std::string_view(workspace.config.project.cache_dir));
        return 1;
    }
    // A live writer's save publishes shards and manifests before the
    // replacement global blob, so a read racing the batch can capture the
    // old global next to newer blobs; the load drops those as stale and
    // the verdicts below misread the mid-write state as damage. The writer
    // may already have finished and unlocked by the time any post-load
    // probe runs, so retry on the drops themselves; genuine damage merely
    // spends the bounded retries before the final no-retry pass reports it.
    view.dropped.assign(loaded.report.reindex().begin(), loaded.report.reindex().end());
    if(allow_retry && !view.dropped.empty()) {
        return open_retry;
    }
    // With no pump attached the load report's debt can only be the
    // recovery drops: every TU's blobs were missing, stale, or corrupt — a
    // damaged cache, not a legitimately empty one.
    if(view.project().manifests.empty() && workspace.shards.empty() && !view.dropped.empty()) {
        LOG_ERROR(
            "Index cache at {} has no servable data ({} translation units need "
            "reindexing); run `clice index` to rebuild",
            std::string_view(workspace.config.project.cache_dir),
            view.dropped.size());
        return 1;
    }
    return 0;
}

}  // namespace

int with_index(llvm::StringRef root,
               llvm::StringRef configuration,
               IndexViewOptions options,
               llvm::function_ref<int(IndexView&)> report) {
    constexpr std::uint32_t attempts = 5;
    for(std::uint32_t attempt = 1; attempt <= attempts; attempt += 1) {
        IndexView view;
        int rc = open_index(view, root, configuration, /*allow_retry=*/attempt < attempts);
        if(rc == open_retry) {
            LOG_DEBUG("Index cache is mid-save; retrying the read");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if(rc != 0) {
            return rc;
        }
        if(options.with_build) {
            load_build(view.workspace, root, view.configuration, view.store.remembered_sources());
        }
        return report(view);
    }
    std::unreachable();
}

}  // namespace clice
