module clice;

import :index.database;
import :index.project_index;
import :project.command_resolver;
import :project.command_snapshot;
import :project.configuration;
import :project.hosting;
import :project.index_reader;
import :project.load;
import :support.logging;
import :vfs.cache_store;
import :vfs.path;

namespace clice {

std::string damaged_index_error(const Project& project) {
    return std::format(
        "the index database at {} is damaged (the log above says where); delete that "
        "directory and run `clice index` to rebuild it",
        index::database_directory(*project.store, project.database_key));
}

Spelling argument_path(const Project& project, llvm::StringRef argument) {
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
    project.resolved_databases = resolved_databases(project.config, configuration);
    project.database_key = database_key(project.config, configuration, project.resolved_databases);
    auto& key = project.database_key;
    project.database = index::open_database(*project.store, key, /*read_only=*/true);
    if(!project.database) {
        // A database that exists but did not open said why as it failed.
        if(!index::database_exists(*project.store, key)) {
            LOG_ERROR("No index cache at {}; run `clice index` first",
                      index::database_directory(*project.store, key));
        }
        return false;
    }
    return true;
}

void log_unusable_format(const Project& project) {
    LOG_ERROR("Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
              std::string_view(project.config.project.cache_dir));
}

void log_unreadable(const Project& project) {
    LOG_ERROR("Failed to read the index cache at {}; the cache was left untouched",
              std::string_view(project.config.project.cache_dir));
}

}  // namespace

IndexReader::IndexReader(ReadOptions options) :
    query(project.index, files, options.check_disk ? &gate : nullptr, nullptr) {}

std::unique_ptr<IndexReader> IndexReader::open(const Spelling& workspace,
                                               Config config,
                                               llvm::StringRef configuration,
                                               ReadOptions options) {
    std::unique_ptr<IndexReader> reader(new IndexReader(options));
    reader->files.spell_root(workspace);
    auto& project = reader->project;
    if(!open_database(project, std::move(config), configuration)) {
        return nullptr;
    }
    if(options.load == IndexLoad::Tables) {
        switch(project.index.open(*project.database)) {
            case index::LoadOutcome::Loaded: return reader;
            case index::LoadOutcome::Empty:
                LOG_ERROR("No index cache at {}; run `clice index` first",
                          std::string_view(project.config.project.cache_dir));
                return nullptr;
            case index::LoadOutcome::Undecodable: log_unusable_format(project); return nullptr;
            case index::LoadOutcome::Unreadable: log_unreadable(project); return nullptr;
        }
        std::unreachable();
    }
    // The writer's load, read-only: it never saves on a read-only database.
    CommandResolver commands{project};
    if(!load_saved(project, commands).decoded) {
        log_unusable_format(project);
        return nullptr;
    }
    // load_saved detaches the database when the header exists but cannot
    // be read — a transient IO error, not an empty index.
    if(project.database == nullptr) {
        log_unreadable(project);
        return nullptr;
    }
    if(options.load == IndexLoad::Build) {
        auto remembered = CommandSnapshot::remembered(project);
        load_build(project,
                   project.config.workspace_root,
                   project.build.active_configuration(),
                   remembered.sources,
                   remembered.provisional);
    }
    return reader;
}

feature::query::Context IndexReader::context() const {
    return {
        .query = query,
        .root = project.config.workspace_root,
        .documentation = project.config.documentation,
    };
}

const clang::LangOptions& IndexReader::dialect(Fid file) {
    auto hosts = ranked_hosts(project, file);
    return clice::dialect(project, file, nullptr, hosts.empty() ? Fid() : hosts.front());
}

std::vector<std::string> IndexReader::stale(std::vector<std::string> listed) const {
    for(auto file: gate.withheld()) {
        listed.emplace_back(files.display(file));
    }
    ranges::sort(listed);
    auto duplicates = ranges::unique(listed);
    listed.erase(duplicates.begin(), duplicates.end());
    return listed;
}

}  // namespace clice
