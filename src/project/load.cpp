module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :index.database;
import :project.configuration;
import :project.load;
import :project.project;
import :support.logging;
import :support.timer;
import :syntax.dependency_graph;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.path;

namespace clice {

ProjectLoad load_project(Project& project,
                         IndexStore& store,
                         CanonicalRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index) {
    ProjectLoad report;
    project.project_index.workspace = root;
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
                project.index_db = index::open_database(*project.store, configuration, true);
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
                    project.index_db = index::open_database(*project.store, configuration);
                    if(!project.index_db) {
                        project.writer_lock.reset();
                    }
                }
            }
            LOG_INFO("Cache store: {}", project.store->base_dir());
            report.opened_store = true;
        }
    }

    auto remembered = store.remembered();
    auto load =
        load_build(project, root, configuration, remembered.sources, remembered.provisional);
    report.has_commands = !load.members.empty() || project.build.declares_sources();
    report.members = std::move(load.members);
    // Persisted index shards are CDB-independent; they load even with no
    // member yet, so a database generated later (picked up by the CDB
    // poll) starts from the previous session's index.
    report.index = store.load({.read_only = read_only_index});
    // A unit deleted while no server ran keeps its index until a look finds
    // it missing (see FreshnessGate).
    project.file_table.disk.find_missing(
        llvm::to_vector(llvm::make_first_range(project.project_index.manifests)));
    return report;
}

std::string deeper_databases(const Project& project) {
    if(project.build.declares_sources()) {
        return {};
    }
    CanonicalRef root = project.config.workspace_root;
    auto& cache_dir = project.config.project.cache_dir;
    auto looked = database_places(root);
    std::vector<std::string> names;
    for(auto& database: compile_commands_below(
            root,
            cache_dir.empty() ? CanonicalPath() : CanonicalPath(Spelling::absolute(cache_dir)))) {
        if(llvm::is_contained(looked, database)) {
            continue;
        }
        auto directory = database.parent();
        auto name = llvm::StringRef(directory).drop_front(root.size()).ltrim(R"(/\)");
        names.push_back(std::format(R"("{}")", llvm::sys::path::convert_to_slash(name)));
    }
    if(names.empty()) {
        return {};
    }
    std::ranges::sort(names);
    return std::format(
        " clice looks at the workspace root and its direct subdirectories unless clice.toml "
        "names a database; to use the deeper ones found, add [[rules]] compile_commands = [{}] "
        "to clice.toml.",
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
    // One source per database, however many of its spellings were found:
    // the root's link to build/compile_commands.json is that database.
    llvm::SmallVector<SourceID> sources;
    for(auto& path: paths) {
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
