module clice;

import :settings.settings;
import :index.database;
import :index.project_index;
import :project.build;
import :project.command_snapshot;
import :project.project;
import :project.save;
import :support.json;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.path;

namespace clice {

namespace {

/// The real path of a declared database file, also of one whose directory
/// does not exist yet: a path without the .json extension names the
/// directory holding it.
std::string real_path(const Spelling& database) {
    if(path::extension(database) == ".json") {
        return CanonicalPath(database).str();
    }
    return CanonicalPath(Spelling("compile_commands.json", database)).str();
}

/// The real paths of the databases the rules active under `configuration`
/// declare, in priority order.
std::vector<std::string> real_databases(const Settings& settings, llvm::StringRef configuration) {
    std::vector<std::string> paths;
    for(auto& database: settings.declared_databases(configuration)) {
        paths.push_back(real_path(database));
    }
    return paths;
}

/// What the rules active under `configuration` do besides naming
/// databases, one string each in declaration order: configurations alike in
/// it and in the databases they read build alike.
std::vector<std::string> edits_of(const Settings& settings, llvm::StringRef configuration) {
    std::vector<std::string> edits;
    for(auto rule: settings.rules) {
        if(!rule.configuration.empty() && rule.configuration != configuration) {
            continue;
        }
        rule.configuration.clear();
        rule.compile_commands.clear();
        rule.resolve_symlinks = false;
        edits.push_back(json::to_string(rule).value_or(""));
    }
    return edits;
}

/// Register and load the build's sources: the databases the rules declare
/// (existing or not; the tracker watches for them), else the ones
/// discovered under the root plus `nearby`, the ones discovery would only
/// meet later — the persisted index's (see CommandSnapshot::remembered),
/// those above the file a run is about (see compile_commands_above).
void load_sources(Project& project, llvm::ArrayRef<Spelling> nearby) {
    CanonicalRef root = project.root;
    ScopedTimer cdb_timer;
    std::size_t entries = 0;
    auto paths = project.build.declared_sources();
    if(!project.build.declares_sources()) {
        paths = discover_compile_commands(root);
        // Registered whether still there or not, like a declared one: the
        // tracker watches for its return, and the index it built keeps
        // serving meanwhile. Sorted like Build::source_order ranks them, so
        // registration order — which the persisted command sequences
        // follow — does not depend on the order files were opened in.
        auto stable = ranges::to<llvm::SmallVector<Spelling>>(
            views::filter(nearby, [&](const Spelling& source) {
                // Where the database sits, as the walks below the root spell
                // it, not what it links to: a compile_commands.json or a build
                // directory symlinked to a tree outside still belongs to the
                // root.
                return path::is_within(llvm::StringRef(source), llvm::StringRef(root));
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
        if(!project.build.declares_sources() && !project.cdb.find_source(path) &&
           project.cdb.find_same_file(path)) {
            continue;
        }
        if(auto id = project.cdb.add_source(path); !ranges::contains(sources, id)) {
            sources.push_back(id);
        }
    }
    if(!project.build.declares_sources() && sources.size() > 1) {
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
            project.build.record(project.file_table.intern(path));
        }
    }

    opened.members = project.build.members();
    if(opened.members.empty()) {
        return;
    }

    auto scan = scan_dependency_graph(project.cdb,
                                      project.toolchain,
                                      project.dep_graph,
                                      project.build.units(opened.members));
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
        opened.members = project.build.members();
    }
}

/// Why the index cache at `where` cannot be read, as `outcome` says; empty
/// for one that loaded.
std::string unreadable_index(index::LoadOutcome outcome, std::string_view where) {
    switch(outcome) {
        case index::LoadOutcome::Loaded: return {};
        case index::LoadOutcome::Empty:
            return std::format("No index cache at {}; run `clice index` first", where);
        case index::LoadOutcome::Undecodable:
            return std::format(
                "Index cache at {} is in an old or corrupt format; run `clice index` to rebuild",
                where);
        case index::LoadOutcome::Unreadable:
            return std::format("Failed to read the index cache at {}; the cache was left untouched",
                               where);
    }
    std::unreachable();
}

/// Open the cache store and the index database as `options` say; why the
/// index cannot be read, for a run that reads it, else empty.
std::string open_store(Project& project, const Project::OpenOptions& options) {
    using Store = Project::OpenOptions::Store;
    using Index = Project::OpenOptions::Index;
    auto& cache_dir = project.settings.project.cache_dir;
    if(options.store == Store::None || cache_dir.empty()) {
        return {};
    }
    bool read_only = options.store == Store::ReadOnly;
    // Read-only, the default cache directory exists as soon as the settings
    // resolves it, so only the versioned store inside it proves an index
    // was ever built — and a live server (even one on an older layout)
    // must not lose blobs to a reader.
    auto store = CacheStore::open(cache_dir, cache_format_version, read_only);
    if(!store) {
        if(read_only && store.error() == std::errc::no_such_file_or_directory) {
            return unreadable_index(index::LoadOutcome::Empty, cache_dir);
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
    if(options.build == Project::OpenOptions::Build::Members) {
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
    if(options.index == Index::Read) {
        // No writer lock: a concurrent server or index run keeps owning it,
        // while the persisted versions and artifact records still seed this
        // run. What it changes stays in memory and exits with it.
        project.database = index::open_database(*project.store, key, /*read_only=*/true);
        if(project.database || !read_only) {
            return {};
        }
        if(!index::database_exists(*project.store, key)) {
            return unreadable_index(index::LoadOutcome::Empty,
                                    index::database_directory(*project.store, key));
        }
        return std::format("The index database at {} cannot be opened; the log above says why",
                           index::database_directory(*project.store, key));
    }
    if(options.index != Index::Write ||
       !(project.writer_lock = index::WriterLock::acquire(cache_dir))) {
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

std::string resolved_databases(const Settings& settings, llvm::StringRef configuration) {
    std::string resolved;
    for(auto& rule: settings.compiled_rules) {
        if(!rule.resolve_symlinks || !rule.active_under(configuration)) {
            continue;
        }
        for(auto& database: rule.compile_commands) {
            if(!resolved.empty()) {
                resolved += '\n';
            }
            resolved += real_path(database);
        }
    }
    return resolved;
}

index::DatabaseKey database_key(const Settings& settings,
                                llvm::StringRef configuration,
                                llvm::StringRef resolved) {
    if(resolved.empty()) {
        return {.configuration = configuration.str()};
    }
    auto databases = real_databases(settings, configuration);
    auto edits = edits_of(settings, configuration);
    for(auto tag: settings.configurations()) {
        auto resolves = [&](const CompiledRule& rule) {
            return rule.resolve_symlinks && rule.active_under(tag);
        };
        if(tag != configuration && ranges::none_of(settings.compiled_rules, resolves) &&
           real_databases(settings, tag) == databases && edits_of(settings, tag) == edits) {
            return {.configuration = tag.str()};
        }
    }
    return {.configuration = configuration.str(), .resolved = resolved.str()};
}

Project::Opened Project::open(const OpenOptions& options) {
    using Build = OpenOptions::Build;
    using Index = OpenOptions::Index;
    assert((options.index != Index::Write || options.store == OpenOptions::Store::Writable) &&
           "the index's writer builds into a writable store");
    Opened opened;
    index.project_root = root;
    commands.set_project_root(root);
    build.reset_active(options.configuration);
    resolved_databases = clice::resolved_databases(settings, options.configuration);
    database_key = clice::database_key(settings, options.configuration, resolved_databases);
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

    auto remembered = CommandSnapshot::remembered(*this);
    if(!options.file.empty()) {
        auto above = compile_commands_above(CanonicalRef(options.file).parent(), root);
        remembered.sources.append(above.begin(), above.end());
    }
    switch(options.build) {
        case Build::None: break;
        case Build::Databases: load_sources(*this, remembered.sources); break;
        case Build::Members:
            load_members(*this, remembered.sources, remembered.provisional, opened);
            break;
    }

    if(database && opened.unreadable.empty()) {
        // A run reading the index alone binds its tables; with the build,
        // the saved state is read as the writer reads it.
        // The tables a run reading the index alone binds in place, fetched
        // on first use.
        auto& cache_dir = settings.project.cache_dir;
        if(options.index == Index::Read && options.build != Build::Members) {
            opened.unreadable = unreadable_index(index.open(*database), cache_dir);
        } else {
            auto saved = load_saved(*this, options.build == Build::Members);
            opened.report = std::move(saved.report);
            // A reader takes no fresh database for one it cannot read.
            if(!saved.decoded && database && database->read_only()) {
                opened.unreadable = unreadable_index(index::LoadOutcome::Undecodable, cache_dir);
            } else if(!database && options.index == Index::Read) {
                opened.unreadable = unreadable_index(index::LoadOutcome::Unreadable, cache_dir);
            }
            // A unit deleted while no server ran keeps its index until a
            // look finds it missing (see FreshnessGate).
            llvm::SmallVector<Fid> units;
            index.for_each_unit([&](Fid tu) { units.push_back(tu); });
            file_table.disk.find_missing(units);
        }
    }
    // The store stays: a run building artifacts (lint without --index)
    // still builds into it, only without the index's records.
    if(!opened.unreadable.empty()) {
        index.clear();
        database.reset();
    }
    return opened;
}

}  // namespace clice
