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
    return Spelling(argument, Spelling(project.config.project_root));
}

IndexReader::IndexReader(ReadOptions options) :
    query(project.index, files, options.check_disk ? &gate : nullptr, nullptr) {}

std::unique_ptr<IndexReader> IndexReader::open(const Spelling& workspace,
                                               Config config,
                                               llvm::StringRef configuration,
                                               ReadOptions options) {
    std::unique_ptr<IndexReader> reader(new IndexReader(options));
    reader->files.spell_root(workspace);
    auto& project = reader->project;
    auto active = resolve_configuration(config, configuration);
    project.config = std::move(config);
    project.index.project_root = project.config.project_root;
    project.build.reset_active(active);
    auto& cache_dir = project.config.project.cache_dir;

    // Without an index the answers are none; a reader that only consults
    // one opens with an empty index instead. Empty `why`: the cause was
    // logged where it failed.
    auto none = [&](std::string why) -> std::unique_ptr<IndexReader> {
        if(options.required) {
            if(!why.empty()) {
                LOG_ERROR("{}", why);
            }
            return nullptr;
        }
        if(!why.empty()) {
            LOG_INFO("{}", why);
        }
        project.index.clear();
        project.database.reset();
        project.store.reset();
        return std::move(reader);
    };
    auto absent = [&](llvm::StringRef where) {
        return none(std::format("No index cache at {}; run `clice index` first", where));
    };
    auto unusable = [&] {
        return none(std::format(
            "Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
            std::string_view(cache_dir)));
    };
    auto unreadable = [&] {
        return none(
            std::format("Failed to read the index cache at {}; the cache was left untouched",
                        std::string_view(cache_dir)));
    };

    // Read-only: the default cache directory exists as soon as the config
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a stats reader.
    auto store = CacheStore::open(cache_dir, cache_format_version, /*read_only=*/true);
    if(!store) {
        if(store.error() == std::errc::no_such_file_or_directory) {
            return absent(cache_dir);
        }
        return none(std::format("Failed to open cache store at {}: {}",
                                std::string_view(cache_dir),
                                store.error().message()));
    }
    project.store.emplace(std::move(*store));
    project.resolved_databases = resolved_databases(project.config, active);
    project.database_key = database_key(project.config, active, project.resolved_databases);
    auto& key = project.database_key;
    project.database = index::open_database(*project.store, key, /*read_only=*/true);
    if(!project.database) {
        // A database that exists but did not open said why as it failed.
        if(!index::database_exists(*project.store, key)) {
            return absent(index::database_directory(*project.store, key));
        }
        return none({});
    }
    if(options.load == IndexLoad::Tables) {
        switch(project.index.open(*project.database)) {
            case index::LoadOutcome::Loaded: return reader;
            case index::LoadOutcome::Empty: return absent(cache_dir);
            case index::LoadOutcome::Undecodable: return unusable();
            case index::LoadOutcome::Unreadable: return unreadable();
        }
        std::unreachable();
    }
    // The writer's load, read-only: it never saves on a read-only database.
    if(!load_saved(project).decoded) {
        return unusable();
    }
    // load_saved detaches the database when the header exists but cannot
    // be read — a transient IO error, not an empty index.
    if(project.database == nullptr) {
        return unreadable();
    }
    if(options.load == IndexLoad::Build) {
        auto remembered = CommandSnapshot::remembered(project);
        load_build(project,
                   project.config.project_root,
                   project.build.active_configuration(),
                   remembered.sources,
                   remembered.provisional);
    }
    return reader;
}

feature::query::Context IndexReader::context() const {
    return {
        .query = query,
        .root = project.config.project_root,
        .documentation = project.config.documentation,
    };
}

const clang::LangOptions& IndexReader::dialect(Fid file) {
    return clice::dialect(project, file, nullptr, host_of(project, file));
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
