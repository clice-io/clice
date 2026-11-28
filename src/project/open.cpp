module clice;

import :command.database;
import :index.database;
import :index.project_index;
import :project.discovery;
import :project.graph.precompiled;
import :project.project;
import :project.saved;
import :settings.settings;
import :support.json;
import :support.logging;
import :support.process;
import :support.text;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;

namespace clice {

Project::Project(FileTable& file_table,
                 CanonicalPath root,
                 async::event_loop* loop,
                 WorkerPool* pool) :
    loop(loop), pool(pool), root(std::move(root)), file_table(file_table) {}

namespace {

/// Of `nearby`, the databases discovery would only meet later — the
/// persisted index's (see SavedMembers::databases), those above the file a
/// run is about (see databases_above) — the ones belonging to the project,
/// in the order Project::source_order ranks them: registration order, which
/// the persisted command sequences follow, does not depend on the order
/// files were opened in.
llvm::SmallVector<Spelling> nearby_sources(const Project& project,
                                           llvm::ArrayRef<Spelling> nearby) {
    CanonicalRef root = project.root;
    auto sources =
        ranges::to<llvm::SmallVector<Spelling>>(views::filter(nearby, [&](const Spelling& source) {
            // Where the database sits, as the walks below the root spell
            // it, not what it links to: a compile_commands.json or a build
            // directory symlinked to a tree outside still belongs to the
            // root, unless a directory on the way is a project of its own.
            return path::is_within(llvm::StringRef(source), llvm::StringRef(root)) &&
                   nested_project(root, CanonicalPath(source)).empty();
        }));
    ranges::sort(sources, {}, [](const Spelling& source) {
        return discovery_rank(llvm::StringRef(source));
    });
    return sources;
}

/// Register and load the build's sources: the databases the rules declare
/// (existing or not; the server watches for them), else the ones
/// discovered under the root plus those of `nearby` (nearby_sources).
void load_sources(Project& project, llvm::ArrayRef<Spelling> nearby) {
    CanonicalRef root = project.root;
    Timer cdb_timer;
    std::size_t entries = 0;
    auto paths = project.rules.declared_databases();
    if(!project.rules.declares_sources()) {
        // Registered whether still there or not, like a declared one: the
        // server watches for its return, and the index it built keeps
        // serving meanwhile.
        paths = discovered_databases(root);
        auto sources = nearby_sources(project, nearby);
        paths.append(sources.begin(), sources.end());
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
            LOG_INFO("Loaded the compilation database {} ({})",
                     project.cdb.source_path(id),
                     counted(*loaded, "entry", "entries"));
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
    // rebuild gives it once the graph tells whether a unit includes it, the
    // builtin one when it borrows none.
    if(project.members().empty() && project.provisional.files().empty()) {
        return;
    }
    auto scan = project.rebuild_dependency_graph().scan;
    opened.members = project.members();

    LOG_PERF("startup",
             "phase=dep_scan files={} edges={} elapsed_ms={}",
             scan.total_files,
             scan.includes_resolved,
             scan.elapsed_ms);
    opened.scan = std::move(scan);
}

/// What a run that cannot write its cache directory is told to do.
constexpr std::string_view writable_cache_dir =
    "give clice write access to it, or set `cache_dir` in clice.toml to a directory it can write";

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

/// Open the index database as its writer under `lock`, recording the
/// project as the cache directory's owner; why it cannot when another
/// project owns the directory or no database opens, else empty.
std::string open_writer(Project& project, index::WriterLock lock) {
    auto& cache_dir = project.settings.cache_dir;
    // The owner is recorded, and checked again, under the lock: another
    // project may have become the directory's owner since the settings
    // chose it.
    CanonicalRef root = project.root;
    auto shown = project.file_table.display(root);
    if(owned_elsewhere(cache_dir, root)) {
        return std::format(
            "Cache directory {} serves the project at {}; set `cache_dir` in the clice.toml of "
            "{} to a directory of its own",
            std::string_view(cache_dir),
            cache_dir_owner(cache_dir),
            shown);
    }
    set_cache_dir_owner(cache_dir, root, shown);
    auto database = index::open_database(*project.store, project.database_key);
    if(!database) {
        return std::move(database.error());
    }
    project.database = std::move(*database);
    project.writer_lock = std::move(lock);
    return {};
}

/// Whether `name`, in `precompiled_dir`, is a project's directory: a cache
/// key, its cache directory's. Nothing else there is ever removed, as the
/// directory may hold other programs' files too (`/tmp`).
bool names_project(llvm::StringRef name) {
    return name.size() == 32 &&
           ranges::all_of(name, [](char c) { return llvm::isDigit(c) || (c >= 'a' && c <= 'f'); });
}

/// Open the store of the project's PCHs and PCMs, its directory in
/// `precompiled_dir`, first removing the directories of the projects there
/// no clice used for precompiled_unused; false, why in `opened`, when a run
/// building into it cannot. A run reading the index finds none when
/// nothing was ever built.
bool open_precompiled_store(Project& project, bool read_only, Project::Opened& opened) {
    auto& settings = project.settings;
    if(settings.precompiled_dir.empty()) {
        if(read_only) {
            return true;
        }
        opened.unwritable =
            "There is no user cache directory for PCHs and PCMs; set `precompiled_dir` in "
            "clice.toml to a directory clice can write";
        return false;
    }
    auto dir = path::join(settings.precompiled_dir, cache_key({settings.cache_identity.str()}));
    if(!read_only) {
        if(auto entries = fs::Directory::open(settings.precompiled_dir)) {
            for(auto& entry: *entries) {
                if(!entry) {
                    break;
                }
                auto other = path::join(settings.precompiled_dir, entry->name);
                if(entry->type == fs::FileType::Directory && other != dir &&
                   names_project(entry->name) &&
                   CacheStore::remove_unused(other, precompiled_unused)) {
                    LOG_INFO("Removed {}: no clice used its PCHs and PCMs for {} days",
                             other,
                             precompiled_unused.count());
                }
            }
        }
    }
    auto store = CacheStore::open(dir, cache_format_version, read_only);
    if(!store) {
        if(read_only) {
            return true;
        }
        opened.unwritable = std::format(
            "Cannot use {} for PCHs and PCMs: {}; give clice write access to it, or "
            "set `precompiled_dir` in clice.toml to a directory it can write",
            settings.precompiled_dir,
            store.error().message());
        return false;
    }
    store->register_namespace({.name = "pch",
                               .extension = ".pch",
                               .aux_extension = ".pch.idx",
                               .max_bytes = settings.cache.pch_bytes,
                               .max_unused = precompiled_unused});
    store->register_namespace({.name = "pcm",
                               .extension = ".pcm",
                               .max_bytes = settings.cache.pcm_bytes,
                               .max_unused = precompiled_unused});
    project.precompiled_store.emplace(std::move(*store));
    if(!read_only) {
        LOG_INFO("PCHs and PCMs: {}", project.precompiled_store->base_dir());
    }
    return true;
}

/// Open the cache store and the index database, and for a run compiling the
/// build's members the store of its PCHs and PCMs, as `options` say; why the
/// index cannot be read, for a run that reads it, or the cache cannot be
/// used, for a run building into it, in `opened`.
void open_store(Project& project, const Project::OpenOptions& options, Project::Opened& opened) {
    using Cache = Project::OpenOptions::Cache;
    auto& cache_dir = project.settings.cache_dir;
    if(options.cache == Cache::None) {
        return;
    }
    bool read_only = options.cache == Cache::Read;
    if(!read_only && project.settings.cache_dir_defaulted) {
        CacheStore::write_ignore_markers(cache_dir);
    }
    // Read-only, the default cache directory exists as soon as the settings
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a reader.
    auto store = CacheStore::open(path::join(cache_dir, "cache"), cache_format_version, read_only);
    if(!store) {
        if(read_only && store.error() == std::errc::no_such_file_or_directory) {
            opened.unreadable = unreadable_index(index::LoadResult::Empty, cache_dir);
            return;
        }
        auto why = std::format("Cannot use the cache directory {}: {}",
                               std::string_view(cache_dir),
                               store.error().message());
        if(read_only) {
            opened.unreadable = std::move(why);
        } else {
            opened.unwritable = std::format("{}; {}", why, writable_cache_dir);
        }
        return;
    }
    // Only a run loading the build's members, or building into the store,
    // compiles them; opening their store scans all of it, which a run
    // reading the index alone does not pay for.
    if((options.load == Project::OpenOptions::Load::Members || options.cache == Cache::Build) &&
       !open_precompiled_store(project, read_only, opened)) {
        return;
    }
    if(!read_only) {
        // An earlier clice kept the PCHs and PCMs beside the index; no clice reads
        // them there any more.
        for(auto name: {"pch", "pcm", "manifest.json"}) {
            store->remove_stale(name);
        }
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
        auto database = index::open_database(*project.store, key, /*read_only=*/true);
        if(database) {
            project.database = std::move(*database);
            return;
        }
        if(!index::database_exists(*project.store, key)) {
            if(read_only) {
                opened.unreadable =
                    unreadable_index(index::LoadResult::Empty,
                                     index::database_directory(*project.store, key));
            }
            return;
        }
        if(!read_only) {
            LOG_WARN("{}; this run reads no index", database.error());
            return;
        }
        opened.unreadable = std::move(database.error());
        return;
    }
    auto lock = index::WriterLock::acquire(cache_dir);
    if(!lock) {
        if(lock.error() == index::WriterLock::Refused::Held) {
            project.index_waits = true;
        } else {
            opened.unwritable = std::format("Cannot open the index writer lock under {}; {}",
                                            std::string_view(cache_dir),
                                            writable_cache_dir);
        }
        return;
    }
    opened.unwritable = open_writer(project, std::move(*lock));
}

/// Load the saved state — the index, the precompiled records, the context
/// choices — `reconcile`d with the build when it is loaded, and mark the
/// units deleted while no run wrote the index missing; why a run that
/// `reads` the index cannot, or why its writer has no database, in
/// `opened`.
void load_saved_state(Project& project, bool reconcile, bool reads, Project::Opened& opened) {
    auto& cache_dir = project.settings.cache_dir;
    index::IndexReport report;
    bool decoded = project.load_saved(reconcile, report);
    project.take_report(report);
    // A reader takes no fresh database for one it cannot read.
    if(!decoded && project.database && project.database->read_only()) {
        opened.unreadable = unreadable_index(index::LoadResult::Undecodable, cache_dir);
        return;
    }
    if(!project.database) {
        if(reads) {
            opened.unreadable = unreadable_index(index::LoadResult::Unreadable, cache_dir);
            return;
        }
        opened.unwritable = std::format(
            "The index database in {} cannot be used (the log above says why); delete its "
            "`index` directory, or set `cache_dir` in clice.toml to another directory",
            std::string_view(cache_dir));
        return;
    }
    // A unit deleted while no server ran keeps its index until a look finds
    // it missing (see FreshnessGate).
    llvm::SmallVector<Fid> units;
    project.index.for_each_unit([&](Fid tu) { units.push_back(tu); });
    project.file_table.disk.find_missing(units);
}

}  // namespace

std::string Project::damaged_index_error() const {
    return std::format(
        "the index database at {} is damaged (the log above says where); delete that "
        "directory and run `clice index` to rebuild it",
        index::database_directory(*store, database_key));
}

void Project::use_configuration(llvm::StringRef configuration) {
    rules = Rules(settings, root, configuration);
    resolved_databases = rules.resolved_databases();
    database_key = rules.database_key(resolved_databases);
}

Project::Opened Project::open(const OpenOptions& options) {
    using Load = OpenOptions::Load;
    using Cache = OpenOptions::Cache;
    Opened opened;
    index.project_root = root;
    commands.set_project_root(root);
    use_configuration(options.configuration);
    freshness.options = options.freshness;
    if(database_key.configuration != options.configuration) {
        LOG_INFO("The databases of configuration {} resolve to those of {}; sharing its index",
                 options.configuration,
                 database_key.configuration);
    } else if(!database_key.resolved.empty()) {
        LOG_INFO("The databases of configuration {} resolve to {}",
                 options.configuration,
                 llvm::join(llvm::split(database_key.resolved, '\n'), ", "));
    }

    open_store(*this, options, opened);

    if(database) {
        saved_members.read(*database);
    }
    auto nearby = saved_members.databases(index);
    for(auto& file: options.files) {
        auto above = databases_above(CanonicalRef(file).parent(), root);
        nearby.append(above.begin(), above.end());
    }
    // LIMITATION: nothing of the load is kept for the next start: every start
    // reads the databases again, queries the compilers again (one process per
    // command group, the slowest on Windows) and scans every file again, for
    // seconds before the first answer on a project of LLVM's size.
    switch(options.load) {
        case Load::Rules: break;
        case Load::Databases: load_sources(*this, nearby); break;
        case Load::Members:
            load_members(*this, nearby, saved_members.provisional(index), opened);
            break;
    }

    if(database && opened.unreadable.empty()) {
        // A run reading the index alone binds its tables in place, fetched
        // on first use; with the build, the saved state is read as the
        // writer reads it.
        bool reads = options.cache != Cache::Write;
        if(reads && options.load != Load::Members) {
            opened.unreadable = unreadable_index(index.open(*database), settings.cache_dir);
        } else {
            load_saved_state(*this, options.load == Load::Members, reads, opened);
        }
    }
    // The store stays: a run building PCHs and PCMs (lint without --index)
    // still builds into it, only without the index's records.
    if(!opened.unreadable.empty()) {
        index.clear();
        database.reset();
    }
    // Marked before any run writes the index: the instantiations they
    // walked hold no more (ProjectIndex::known_instantiations), whether the
    // background index rebuilds them or not.
    if(options.cache == Cache::Write) {
        index.stale_units();
    }
    return opened;
}

async::task<std::optional<index::WriterLock>> Project::wait_for_writer() {
    auto wait = chrono::milliseconds(env_integer("CLICE_TEST_WRITER_WAIT_MS").value_or(5'000));
    auto deadline = chrono::steady_clock::now() + wait;
    while(true) {
        auto lock = index::WriterLock::acquire(settings.cache_dir);
        if(lock) {
            co_return std::move(*lock);
        }
        if(lock.error() != index::WriterLock::Refused::Held ||
           chrono::steady_clock::now() >= deadline) {
            co_return std::nullopt;
        }
        // No event: a lock let go of in another process tells no one.
        co_await async::sleep(chrono::milliseconds(100));
    }
}

CDBDiff Project::open_index(index::WriterLock lock, Opened& opened) {
    index_waits = false;
    CDBDiff joined;
    opened.unwritable = open_writer(*this, std::move(lock));
    if(!database) {
        return joined;
    }
    saved_members.read(*database);
    if(!rules.declares_sources()) {
        for(auto& source: nearby_sources(*this, saved_members.databases(index))) {
            if(!cdb.find_source(source) && !cdb.find_same_file(source)) {
                if(auto diff = cdb.reload_and_diff(cdb.add_source(source))) {
                    joined.append(*diff);
                }
            }
        }
    }
    bool recorded = false;
    for(auto& path: saved_members.provisional(index)) {
        if(fs::is_file(path)) {
            recorded = record_provisional(file_table.intern(path)) || recorded;
        }
    }
    if(!joined.empty() || recorded) {
        joined.append(rebuild_dependency_graph().provisional);
    }
    load_saved_state(*this, true, false, opened);
    index.stale_units();
    opened.members = members();
    return joined;
}

}  // namespace clice
