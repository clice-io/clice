module clice;

import :command.database;
import :index.database;
import :index.project_index;
import :project.discovery;
import :project.listings;
import :project.project;
import :settings.settings;
import :support.json;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;

namespace clice {

Project::Project(FileTable& file_table,
                 CanonicalPath root,
                 async::event_loop* loop,
                 WorkerPool* pool) :
    loop(loop), pool(pool), root(std::move(root)), file_table(file_table) {}

namespace {

/// Register and load the build's sources: the databases the rules declare
/// (existing or not; the server watches for them), else the ones
/// discovered under the root plus `nearby`, the ones discovery would only
/// meet later — the persisted index's (see SavedListings::databases),
/// those above the file a run is about (see databases_above).
void load_sources(Project& project, llvm::ArrayRef<Spelling> nearby) {
    CanonicalRef root = project.root;
    ScopedTimer cdb_timer;
    std::size_t entries = 0;
    auto paths = project.rules.declared_databases();
    if(!project.rules.declares_sources()) {
        paths = discovered_databases(root);
        // Registered whether still there or not, like a declared one: the
        // server watches for its return, and the index it built keeps
        // serving meanwhile. Sorted like Project::source_order ranks them, so
        // registration order — which the persisted command sequences
        // follow — does not depend on the order files were opened in.
        auto stable = ranges::to<llvm::SmallVector<Spelling>>(
            views::filter(nearby, [&](const Spelling& source) {
                // Where the database sits, as the walks below the root spell
                // it, not what it links to: a compile_commands.json or a build
                // directory symlinked to a tree outside still belongs to the
                // root, unless a directory on the way is a project of its
                // own.
                return path::is_within(llvm::StringRef(source), llvm::StringRef(root)) &&
                       nested_project(root, CanonicalPath(source)).empty();
            }));
        ranges::sort(stable, {}, [](const Spelling& source) {
            return discovery_rank(llvm::StringRef(source));
        });
        paths.append(stable.begin(), stable.end());
    }
    // One source per database, however many of its spellings discovery
    // found: the root's link to build/compile_commands.json is that
    // database. A rule's spelling stays its own, which its patterns rank.
    llvm::SmallVector<SourceID> sources;
    for(auto& path: paths) {
        if(!project.rules.declares_sources() && !project.cdb.find_source(path) &&
           project.cdb.find_same_file(path)) {
            continue;
        }
        if(auto id = project.cdb.add_source(path); !ranges::contains(sources, id)) {
            sources.push_back(id);
        }
    }
    if(!project.rules.declares_sources() && sources.size() > 1) {
        LOG_WARN(
            "No rule names a compilation database; the {} found apply in this order, "
            "an earlier one winning for a file both list: {}. To switch between them "
            "instead, declare each on a tagged rule: [[rules]] configuration = \"...\" "
            "compile_commands = [\"...\"]",
            sources.size(),
            llvm::join(
                views::transform(sources, [&](SourceID id) { return project.cdb.source_path(id); }),
                ", "));
    }
    for(auto id: sources) {
        if(auto loaded = project.cdb.load_source(id)) {
            LOG_INFO("Loaded CDB from {} with {} entries", project.cdb.source_path(id), *loaded);
            entries += *loaded;
        } else if(!fs::exists(project.cdb.source_path(id))) {
            LOG_WARN("Compilation database {} does not exist yet", project.cdb.source_path(id));
        }
    }
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", entries, cdb_timer.ms());
}

/// load_sources, then record the `provisional` members the persisted index
/// remembers, enumerate the build's members and scan the dependency graph
/// from them.
void load_members(Project& project,
                  llvm::ArrayRef<Spelling> nearby,
                  llvm::ArrayRef<Spelling> provisional,
                  Project::Opened& opened) {
    load_sources(project, nearby);

    // Only a restart forgets the record of a file gone since: during a
    // session a deleted provisional member keeps it, as a vanished unit
    // keeps its entry, and is back the moment the file is.
    for(auto& path: provisional) {
        if(fs::is_file(path)) {
            project.record_provisional(project.file_table.intern(path));
        }
    }

    // A recorded file is a member once it has its command, which the
    // refresh below gives it, the builtin one when no unit lends one.
    opened.members = project.members();
    if(opened.members.empty() && project.provisional.files().empty()) {
        return;
    }

    auto scan = scan_dependency_graph(project.cdb,
                                      project.toolchain,
                                      project.dep_graph,
                                      project.scan_commands(opened.members));
    project.dep_graph.build_reverse_map();

    auto found = scan.includes_resolved + scan.unresolved.size();
    double accuracy =
        found > 0 ? 100.0 * static_cast<double>(scan.includes_resolved) / found : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header, {} reused), "
        "{}/{} includes resolved ({:.1f}%), {} waves",
        scan.elapsed_ms,
        scan.total_files,
        scan.source_files,
        scan.total_files - scan.source_files,
        scan.scan_cache_hits,
        scan.includes_resolved,
        found,
        accuracy,
        scan.waves);
    LOG_PERF("startup",
             "phase=dep_scan files={} edges={} elapsed_ms={}",
             scan.total_files,
             scan.includes_resolved,
             scan.elapsed_ms);
    opened.scan = std::move(scan);

    // The recorded files join against the scanned graph, which alone tells
    // whether a unit includes one, and the graph takes them in.
    if(!project.refresh_provisional().empty()) {
        project.rebuild_dependency_graph();
        opened.members = project.members();
    }
}

/// Why the index cache at `where` cannot be read, as `result` says; empty
/// for one that loaded.
std::string unreadable_index(index::LoadResult result, std::string_view where) {
    switch(result) {
        case index::LoadResult::Loaded: return {};
        case index::LoadResult::Empty:
            return std::format("No index cache at {}; run `clice index` first", where);
        case index::LoadResult::Undecodable:
            return std::format(
                "Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                where);
        case index::LoadResult::Unreadable:
            return std::format("Failed to read the index cache at {}; the cache was left untouched",
                               where);
    }
    std::unreachable();
}

/// Open the cache store and the index database as `options` say; why the
/// index cannot be read, for a run that reads it, else empty.
std::string open_store(Project& project, const Project::OpenOptions& options) {
    using Cache = Project::OpenOptions::Cache;
    auto& cache_dir = project.settings.project.cache_dir;
    if(options.cache == Cache::None || cache_dir.empty()) {
        return {};
    }
    bool read_only = options.cache == Cache::Read;
    // Read-only, the default cache directory exists as soon as the settings
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a reader.
    auto store = CacheStore::open(cache_dir, cache_format_version, read_only);
    if(!store) {
        if(read_only && store.error() == std::errc::no_such_file_or_directory) {
            return unreadable_index(index::LoadResult::Empty, cache_dir);
        }
        auto why = std::format("Failed to open cache store at {}: {}",
                               std::string_view(cache_dir),
                               store.error().message());
        if(read_only) {
            return why;
        }
        LOG_WARN("{}", why);
        return {};
    }
    // Only a run loading the build's members compiles them; registering
    // scans the whole namespace, which a run reading the index alone
    // does not pay for. Size budgets are deliberately generous: eviction
    // exists to bound disk usage, not to keep the working set tight.
    if(options.load == Project::OpenOptions::Load::Members) {
        constexpr std::uint64_t GiB = 1ull << 30;
        store->register_namespace({.name = "pch",
                                   .extension = ".pch",
                                   .aux_extension = ".pch.idx",
                                   .max_bytes = 8 * GiB});
        store->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 8 * GiB});
    }
    project.store.emplace(std::move(*store));
    if(!read_only) {
        LOG_INFO("Cache store: {}", project.store->base_dir());
    }

    auto& key = project.database_key;
    if(options.cache != Cache::Write) {
        // No writer lock: a concurrent server or index run keeps owning it,
        // while the persisted versions and precompiled records still seed this
        // run. What it changes stays in memory and exits with it.
        project.database = index::open_database(*project.store, key, /*read_only=*/true);
        if(project.database || !read_only) {
            return {};
        }
        if(!index::database_exists(*project.store, key)) {
            return unreadable_index(index::LoadResult::Empty,
                                    index::database_directory(*project.store, key));
        }
        return std::format("The index database at {} cannot be opened; the log above says why",
                           index::database_directory(*project.store, key));
    }
    if(!(project.writer_lock = index::WriterLock::acquire(cache_dir))) {
        return {};
    }
    // The owner is recorded, and checked again, under the lock: another
    // project may have become the directory's owner since the settings
    // chose it.
    CanonicalRef root = project.root;
    if(owned_elsewhere(cache_dir, root)) {
        LOG_GUIDANCE("Cache directory {} serves the project at {}; {} keeps no index",
                     std::string_view(cache_dir),
                     cache_dir_owner(cache_dir),
                     root);
        project.writer_lock.reset();
        return {};
    }
    set_cache_dir_owner(cache_dir, root);
    project.database = index::open_database(*project.store, key);
    if(!project.database) {
        project.writer_lock.reset();
    }
    return {};
}

}  // namespace

std::string Project::damaged_index_error() const {
    return std::format(
        "the index database at {} is damaged (the log above says where); delete that "
        "directory and run `clice index` to rebuild it",
        index::database_directory(*store, database_key));
}

Project::Opened Project::open(const OpenOptions& options) {
    using Load = OpenOptions::Load;
    using Cache = OpenOptions::Cache;
    Opened opened;
    index.project_root = root;
    commands.set_project_root(root);
    rules = Rules(settings, root, options.configuration);
    freshness.options = options.freshness;
    resolved_databases = rules.resolved_databases();
    database_key = rules.database_key(resolved_databases);
    if(database_key.configuration != options.configuration) {
        LOG_INFO("The databases of configuration {} resolve to those of {}; sharing its index",
                 options.configuration,
                 database_key.configuration);
    } else if(!database_key.resolved.empty()) {
        LOG_INFO("The databases of configuration {} resolve to {}",
                 options.configuration,
                 llvm::join(llvm::split(database_key.resolved, '\n'), ", "));
    }

    opened.unreadable = open_store(*this, options);

    if(database) {
        listings.read(*database);
    }
    auto nearby = listings.databases(index);
    if(!options.file.empty()) {
        auto above = databases_above(CanonicalRef(options.file).parent(), root);
        nearby.append(above.begin(), above.end());
    }
    switch(options.load) {
        case Load::Rules: break;
        case Load::Databases: load_sources(*this, nearby); break;
        case Load::Members: load_members(*this, nearby, listings.provisional(index), opened); break;
    }

    if(database && opened.unreadable.empty()) {
        // A run reading the index alone binds its tables; with the build,
        // the saved state is read as the writer reads it.
        // The tables a run reading the index alone binds in place, fetched
        // on first use.
        auto& cache_dir = settings.project.cache_dir;
        bool reads = options.cache != Cache::Write;
        if(reads && options.load != Load::Members) {
            opened.unreadable = unreadable_index(index.open(*database), cache_dir);
        } else {
            bool decoded = load_saved(options.load == Load::Members, opened.report);
            // A reader takes no fresh database for one it cannot read.
            if(!decoded && database && database->read_only()) {
                opened.unreadable = unreadable_index(index::LoadResult::Undecodable, cache_dir);
            } else if(!database && reads) {
                opened.unreadable = unreadable_index(index::LoadResult::Unreadable, cache_dir);
            }
            // A unit deleted while no server ran keeps its index until a
            // look finds it missing (see FreshnessGate).
            llvm::SmallVector<Fid> units;
            index.for_each_unit([&](Fid tu) { units.push_back(tu); });
            file_table.disk.find_missing(units);
        }
    }
    // The store stays: a run building PCHs and PCMs (lint without --index)
    // still builds into it, only without the index's records.
    if(!opened.unreadable.empty()) {
        index.clear();
        database.reset();
    }
    return opened;
}

}  // namespace clice
