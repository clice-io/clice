module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :index.database;
import :index.project_index;
import :index.tu_manifest;
import :project.artifact_records;
import :project.command_resolver;
import :project.command_snapshot;
import :project.configuration;
import :project.load;
import :project.project;
import :project.save;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

/// Drop the index of every TU a rule's `index = false` keeps out: an index
/// built before the rule would otherwise serve on, since the pump never
/// rebuilds an excluded unit.
void retire_excluded(Project& project, index::IndexReport& report) {
    llvm::SmallVector<Fid> excluded;
    project.index.for_each_manifest([&](Fid tu, const index::TUManifest&) {
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

}  // namespace

SavedLoad load_saved(Project& project, CommandResolver& commands, index::LoadOptions options) {
    SavedLoad result;
    auto& report = result.report;
    if(!project.database) {
        return result;
    }
    auto& db = *project.database;
    auto& index = project.index;
    bool read_only = options.read_only;
    ScopedTimer timer;

    // Artifact metadata and context choices are index-independent: they
    // load (and self-validate) whatever happened to the project table.
    auto load_metadata = [&] {
        if(auto artifacts = db.read(index::BlobKind::ArtifactRecords, "artifacts")) {
            load_artifact_records(project, commands, artifacts.buffer->getBuffer());
        }
        if(auto choices = db.read(index::BlobKind::ContextChoices, "contexts")) {
            project.contexts.bytes = choices.buffer->getBuffer().str();
        }
    };

    // Everything a read-only session keeps is deserialized or copied, so
    // nothing borrows the opening snapshot; retiring it keeps a long
    // read-only session (batch lint) from pinning a concurrent writer's
    // page reclamation to session start. Every return path that leaves
    // the database open owes this.
    auto retire_snapshot = [&] {
        if(read_only && !options.borrow && db.advance_read_snapshot()) {
            db.retire_old_snapshot();
        }
    };

    switch(index.load(db, options, report)) {
        case index::LoadOutcome::Empty:
            load_metadata();
            retire_snapshot();
            return result;
        case index::LoadOutcome::Unreadable:
            // Run this session memory-only — saving a fresh lineage over
            // blobs whose anchor was never read could alias their version
            // ids and generations — and leave everything for a healthier
            // restart to load. Confirmed page corruption heals through
            // rebuildability instead: the condemned database deletes
            // itself and a fresh empty one opens in its place, so this
            // session's rebuild persists instead of being redone at the
            // next start.
            if(!read_only && db.corrupted()) {
                reopen_fresh_database(project);
            } else {
                // Best effort before detaching: the metadata blobs may
                // still read while only the project table is unreadable.
                load_metadata();
                LOG_WARN(
                    "Index project table unreadable; disabling index persistence this session");
                project.database.reset();
            }
            return result;
        case index::LoadOutcome::Undecodable:
            load_metadata();
            result.decoded = false;
            retire_snapshot();
            return result;
        case index::LoadOutcome::Loaded: break;
    }
    load_metadata();
    if(!read_only) {
        retire_excluded(project, report);
        project.command_snapshot.reconcile(project, report);
    }

    // The reads above touch every adopted manifest and shard, so page
    // corruption anywhere in the database has latched by now; heal it like
    // the unreadable-table case above instead of writing into a damaged
    // tree every session. The adopted state unwinds wholesale: the shard
    // views borrow from the condemned environment, and manifests kept
    // without their views would read as fresh and gate the rebuild pass
    // off exactly the files whose shards were lost. Standalone-indexed TUs
    // re-enqueue first — the CDB pass that rebuilds everything else never
    // covers them, and the condemned database is deleting their only
    // persistent record. The dirtied snapshot carries them as debt from
    // the fresh database's first save on, so even a crash before their
    // rebuild finishes cannot lose them a second time.
    if(!read_only && db.corrupted()) {
        index.for_each_manifest([&](Fid tu, const index::TUManifest&) {
            if(project.build.entries(tu).empty()) {
                report.add_reindex(tu);
            }
        });
        index.clear();
        project.command_snapshot.forget_persisted();
        reopen_fresh_database(project);
        return result;
    }

    retire_snapshot();

    if(index.shard_count() != 0) {
        LOG_INFO("Loaded {} index shards, {} manifests, {} symbols",
                 index.shard_count(),
                 index.manifest_count(),
                 index.symbol_count());
    }
    LOG_PERF("startup",
             "phase=index_load symbols={} shards={} manifests={} elapsed_ms={}",
             index.symbol_count(),
             index.shard_count(),
             index.manifest_count(),
             timer.ms());
    return result;
}

ProjectLoad load_project(Project& project,
                         CommandResolver& commands,
                         CanonicalRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index) {
    ProjectLoad report;
    project.index.workspace = root;
    auto& cfg = project.config.project;
    auto configuration = resolve_configuration(project.config, requested_configuration);

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
                project.database = index::open_database(*project.store, configuration, true);
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
                    project.database = index::open_database(*project.store, configuration);
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
    report.index = load_saved(project, commands, {.read_only = read_only_index});
    // A unit deleted while no server ran keeps its index until a look finds
    // it missing (see FreshnessGate).
    llvm::SmallVector<Fid> units;
    project.index.for_each_manifest([&](Fid tu, const index::TUManifest&) { units.push_back(tu); });
    project.file_table.disk.find_missing(units);
    return report;
}

std::string deeper_databases_hint(const Project& project) {
    if(project.build.declares_sources()) {
        return {};
    }
    CanonicalRef root = project.config.workspace_root;
    auto& cache_dir = project.config.project.cache_dir;
    std::vector<std::string> names;
    for(auto& database: deeper_compile_commands(
            root,
            cache_dir.empty() ? CanonicalPath() : CanonicalPath(Spelling::absolute(cache_dir)))) {
        auto directory = database.parent();
        auto name = llvm::StringRef(directory).drop_front(root.size()).ltrim(R"(/\)");
        names.push_back(std::format(R"("{}")", llvm::sys::path::convert_to_slash(name)));
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

BuildLoad load_build(Project& project,
                     CanonicalRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<Spelling> nearby,
                     llvm::ArrayRef<Spelling> provisional) {
    BuildLoad load;
    project.cdb.set_workspace_root(root);
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
        auto stable = llvm::to_vector(llvm::make_filter_range(nearby, [&](const Spelling& source) {
            // Where the database sits, as the walks below the root spell
            // it, not what it links to: a compile_commands.json or a build
            // directory symlinked to a tree outside still belongs to the
            // root.
            return path::under(llvm::StringRef(source), llvm::StringRef(root));
        }));
        auto key = [](const Spelling& source) {
            return std::tuple(
                llvm::count_if(source.str(), [](char c) { return path::is_separator(c); }),
                llvm::StringRef(source));
        };
        std::ranges::sort(stable, {}, key);
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
        if(auto id = project.cdb.add_source(path); !llvm::is_contained(sources, id)) {
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
                llvm::map_range(sources, [&](SourceID id) { return project.cdb.source_path(id); }),
                ", "));
    }
    for(auto id: sources) {
        if(auto loaded = project.cdb.load_source(id)) {
            LOG_INFO("Loaded CDB from {} with {} entries", project.cdb.source_path(id), *loaded);
            entries += *loaded;
        } else {
            LOG_WARN("Compilation database {} is not readable yet", project.cdb.source_path(id));
        }
    }
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", entries, cdb_timer.ms());

    // Only a restart forgets the record of a file gone since: during a
    // session a deleted provisional member keeps it, as a vanished unit
    // keeps its entry, and is back the moment the file is.
    for(auto& path: provisional) {
        if(vfs::is_file(path)) {
            project.build.record(project.file_table.intern(path));
        }
    }

    load.members = project.build.members();
    if(load.members.empty()) {
        return load;
    }

    auto scan =
        scan_dependency_graph(project.cdb, project.dep_graph, project.build.units(load.members));
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
