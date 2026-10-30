module clice;

import :index.database;
import :index.project_index;
import :project.artifact_records;
import :project.build;
import :project.command_snapshot;
import :project.configuration;
import :project.load;
import :project.project;
import :project.save;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.path;

namespace clice {

/// Drop the index of every TU a rule's `index = false` keeps out: an index
/// built before the rule would otherwise serve on, since the pump never
/// rebuilds an excluded unit.
static void retire_excluded(Project& project, index::IndexReport& report) {
    llvm::SmallVector<Fid> excluded;
    project.index.for_each_unit([&](Fid tu) {
        if(!project.build.indexed(project.file_table.resolve(tu))) {
            excluded.push_back(tu);
        }
    });
    for(auto tu: excluded) {
        LOG_INFO("A rule keeps {} out of the index; dropping its entries",
                 project.file_table.resolve(tu));
        project.index.drop(tu, report);
    }
}

SavedLoad load_saved(Project& project) {
    SavedLoad result;
    auto& report = result.report;
    if(!project.database) {
        return result;
    }
    auto& db = *project.database;
    auto& index = project.index;
    bool read_only = db.read_only();
    ScopedTimer timer;

    // Artifact metadata and context choices are index-independent: they
    // load (and self-validate) beside any index state.
    auto load_metadata = [&] {
        if(auto artifacts = db.read(index::BlobKind::ArtifactRecords, "artifacts")) {
            load_artifact_records(project, artifacts.buffer->getBuffer());
        }
        if(auto choices = db.read(index::BlobKind::ContextChoices, "contexts")) {
            project.contexts.bytes = choices.buffer->getBuffer().str();
        }
    };

    switch(index.open(db)) {
        // A database with no index yet may still hold the command snapshot,
        // and with it the debt of the files a save owed an index: the
        // snapshot of a session whose index was discarded.
        case index::LoadOutcome::Empty: break;
        case index::LoadOutcome::Unreadable:
            // Run this session memory-only — saving a fresh lineage over
            // records whose header was never read could alias their path
            // ids — and leave everything for a healthier restart to load.
            // Confirmed page corruption heals through rebuildability
            // instead: the condemned database deletes itself and a fresh
            // empty one opens in its place, so this session's rebuild
            // persists instead of being redone at the next start.
            if(!read_only && db.corrupted()) {
                LOG_WARN("Index database is corrupt; discarding it and rebuilding from scratch");
                reopen_fresh_database(project);
            } else {
                // Best effort before detaching: the metadata blobs may
                // still read while only the header is unreadable.
                load_metadata();
                LOG_WARN(
                    "Index database header unreadable; disabling index persistence this "
                    "session");
                project.database.reset();
            }
            return result;
        case index::LoadOutcome::Undecodable:
            // Another format discards everything, the metadata with it.
            result.decoded = false;
            if(!read_only) {
                reopen_fresh_database(project);
            }
            return result;
        case index::LoadOutcome::Loaded: break;
    }
    load_metadata();
    if(!read_only) {
        retire_excluded(project, report);
        project.command_snapshot.reconcile(project, report);
    }

    // The reads above may have met a damaged page or record: heal it like
    // the unreadable-header case above instead of writing into a damaged
    // tree every session. The session starts over as a first run.
    if(!read_only && index.damaged()) {
        recover_corrupt_database(project, report);
        return result;
    }

    LOG_PERF("startup", "phase=index_load elapsed_ms={}", timer.ms());
    return result;
}

ProjectLoad load_project(Project& project,
                         CanonicalRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index) {
    ProjectLoad report;
    project.index.project_root = root;
    auto& cfg = project.config.project;
    auto configuration = resolve_configuration(project.config, requested_configuration);
    project.resolved_databases = resolved_databases(project.config, configuration);
    project.database_key = database_key(project.config, configuration, project.resolved_databases);
    auto& key = project.database_key;
    if(key.configuration != configuration) {
        LOG_INFO("The databases of configuration {} resolve to those of {}; sharing its index",
                 configuration,
                 key.configuration);
    } else if(!key.resolved.empty()) {
        LOG_INFO("The databases of configuration {} resolve to {}",
                 configuration,
                 llvm::join(llvm::split(key.resolved, '\n'), ", "));
    }

    if(!project.store && !cfg.cache_dir.empty()) {
        auto cache = CacheStore::open(cfg.cache_dir, cache_format_version);
        if(!cache) {
            LOG_WARN("Failed to open cache store at {}: {}",
                     std::string_view(cfg.cache_dir),
                     cache.error().message());
        } else {
            // Size budgets are deliberately generous: eviction exists to
            // bound disk usage, not to keep the working set tight.
            constexpr std::uint64_t GiB = 1ull << 30;
            cache->register_namespace({.name = "pch",
                                       .extension = ".pch",
                                       .aux_extension = ".pch.idx",
                                       .max_bytes = 8 * GiB});
            cache->register_namespace({.name = "pcm", .extension = ".pcm", .max_bytes = 8 * GiB});
            project.store.emplace(std::move(*cache));
            // A read-only bootstrap opens the index database read-only:
            // no writer lock (a concurrent server or index run keeps
            // owning it), while the persisted versions and artifact
            // metadata still seed this session. Its own metadata stays in
            // memory and exits with it.
            if(read_only_index) {
                project.database = index::open_database(*project.store, key, true);
            } else if((project.writer_lock = index::WriterLock::acquire(cfg.cache_dir))) {
                // The owner is recorded, and checked again, under the lock:
                // another project may have become the directory's owner since
                // the configuration chose it.
                if(owned_elsewhere(cfg.cache_dir, root)) {
                    LOG_GUIDANCE("Cache directory {} serves the project at {}; {} keeps no index",
                                 std::string_view(cfg.cache_dir),
                                 cache_dir_owner(cfg.cache_dir),
                                 root);
                    project.writer_lock.reset();
                } else {
                    set_cache_dir_owner(cfg.cache_dir, root);
                    project.database = index::open_database(*project.store, key);
                    if(!project.database) {
                        project.writer_lock.reset();
                    }
                }
            }
            LOG_INFO("Cache store: {}", project.store->base_dir());
            report.opened_store = true;
        }
    }

    auto remembered = CommandSnapshot::remembered(project);
    auto load =
        load_build(project, root, configuration, remembered.sources, remembered.provisional);
    report.has_commands = !load.members.empty() || project.build.declares_sources();
    report.members = std::move(load.members);
    // Persisted index shards are CDB-independent; they load even with no
    // member yet, so a database generated later (picked up by the CDB
    // poll) starts from the previous session's index.
    report.index = load_saved(project);
    // A unit deleted while no server ran keeps its index until a look finds
    // it missing (see FreshnessGate).
    llvm::SmallVector<Fid> units;
    project.index.for_each_unit([&](Fid tu) { units.push_back(tu); });
    project.file_table.disk.find_missing(units);
    return report;
}

std::string deeper_databases_hint(const Project& project) {
    if(project.build.declares_sources()) {
        return {};
    }
    CanonicalRef root = project.config.project_root;
    auto& cache_dir = project.config.project.cache_dir;
    std::vector<std::string> names;
    for(auto& database: deeper_compile_commands(
            root,
            cache_dir.empty() ? CanonicalPath() : CanonicalPath(Spelling::absolute(cache_dir)))) {
        auto directory = database.parent();
        auto name = llvm::StringRef(directory).drop_front(root.size()).ltrim(R"(/\)").str();
        path::to_slash(name);
        names.push_back(std::format(R"("{}")", name));
    }
    if(names.empty()) {
        return {};
    }
    return std::format(
        " Deeper ones load only when clice.toml names them (the editor also loads one when it "
        "opens a file below it); to use those found, give clice.toml a [[rules]] table with "
        "compile_commands = [{}].",
        llvm::join(names, ", "));
}

bool found_unreadable_database(const Project& project) {
    return ranges::any_of(views::iota(std::size_t(0), project.cdb.source_count()),
                          [&](std::size_t i) {
                              return !project.cdb.loaded(SourceID(i)) &&
                                     fs::exists(project.cdb.source_path(SourceID(i)));
                          });
}

std::string why_no_members(const Project& project, llvm::StringRef root) {
    std::vector<std::string> empty;
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        auto path = project.cdb.source_path(SourceID(i));
        if(project.cdb.loaded(SourceID(i))) {
            empty.push_back(path.str());
        } else if(fs::exists(path)) {
            return std::format("{} cannot be read; the error above says why.", path.str());
        } else {
            return std::format("{} does not exist yet; the build writes it.", path.str());
        }
    }
    if(!empty.empty()) {
        return std::format("{} {} no file.",
                           llvm::join(empty, ", "),
                           empty.size() == 1 ? "lists" : "list");
    }
    if(project.build.declares_sources()) {
        return "no file on disk matches a rule with a default_command.";
    }
    return std::format("no compile_commands.json found in {} or its direct subdirectories.{}",
                       root.str(),
                       deeper_databases_hint(project));
}

void load_sources(Project& project,
                  CanonicalRef root,
                  llvm::StringRef configuration,
                  llvm::ArrayRef<Spelling> nearby) {
    project.commands.set_project_root(root);
    project.build.reset_active(configuration);

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

BuildLoad load_build(Project& project,
                     CanonicalRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<Spelling> nearby,
                     llvm::ArrayRef<Spelling> provisional) {
    BuildLoad load;
    load_sources(project, root, configuration, nearby);

    // Only a restart forgets the record of a file gone since: during a
    // session a deleted provisional member keeps it, as a vanished unit
    // keeps its entry, and is back the moment the file is.
    for(auto& path: provisional) {
        if(fs::is_file(path)) {
            project.build.record(project.file_table.intern(path));
        }
    }

    load.members = project.build.members();
    if(load.members.empty()) {
        return load;
    }

    auto scan = scan_dependency_graph(project.cdb,
                                      project.toolchain,
                                      project.dep_graph,
                                      project.build.units(load.members));
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
    load.scan = std::move(scan);

    // The recorded files join against the scanned graph, which alone tells
    // whether a unit includes one, and the graph takes them in.
    if(!project.refresh_provisional().empty()) {
        project.rebuild_dependency_graph();
        load.members = project.build.members();
    }
    return load;
}

}  // namespace clice
