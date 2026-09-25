#include "project/load.h"

#include <string>
#include <vector>

#include "index/database.h"
#include "project/configuration.h"
#include "project/project.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/dependency_graph.h"

namespace clice {

ProjectLoad load_project(Project& project,
                         IndexStore& store,
                         CanonicalRef root,
                         llvm::StringRef requested_configuration,
                         bool read_only_index,
                         bool scan_tree) {
    ProjectLoad report;
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
                                       .policy = CachePolicy::LRU,
                                       .max_bytes = 8 * GiB});
            cache->register_namespace({.name = "pcm",
                                       .extension = ".pcm",
                                       .policy = CachePolicy::LRU,
                                       .max_bytes = 8 * GiB});
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
                // another project may have claimed the directory since the
                // configuration chose it.
                if(owned_elsewhere(cfg.cache_dir, root)) {
                    LOG_GUIDANCE("Cache directory {} serves the project at {}; {} keeps no index",
                                 std::string_view(cfg.cache_dir),
                                 cache_dir_owner(cfg.cache_dir),
                                 root);
                    project.writer_lock.reset();
                } else {
                    claim_cache_dir(cfg.cache_dir, root);
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

    auto nearby = store.remembered_sources();
    if(scan_tree) {
        project.build.reset_active(configuration);
        if(!project.build.declares_sources()) {
            auto below = compile_commands_below(root, cfg.cache_dir);
            nearby.insert(nearby.end(), below.begin(), below.end());
        }
    }
    auto load = load_build(project, root, configuration, nearby);
    report.has_commands = !load.members.empty() || project.build.declares_sources();
    report.members = std::move(load.members);
    // Persisted index shards are CDB-independent; they load even with no
    // member yet, so a database generated later (picked up by the CDB
    // poll) starts from the previous session's index.
    report.index = store.load({.read_only = read_only_index});
    return report;
}

BuildLoad load_build(Project& project,
                     CanonicalRef root,
                     llvm::StringRef configuration,
                     llvm::ArrayRef<std::string> nearby) {
    BuildLoad load;
    project.cdb.set_workspace_root(root);
    project.build.reset_active(configuration);

    ScopedTimer cdb_timer;
    std::size_t entries = 0;
    llvm::SmallVector<std::string> paths;
    for(auto declared: project.build.declared_sources()) {
        paths.push_back(declared.str());
    }
    if(!project.build.declares_sources()) {
        paths = discover_compile_commands(root);
        // Registered whether still there or not, like a declared one: the
        // tracker watches for its return, and the index it built keeps
        // serving meanwhile. Sorted like Build::source_order ranks them, so
        // registration order — which the persisted command sequences
        // follow — does not depend on the order files were opened in.
        auto stable =
            llvm::to_vector(llvm::make_filter_range(nearby, [&](const std::string& source) {
                // Where the database sits, not what it links to: a
                // compile_commands.json symlinked to a build tree outside
                // still belongs to the root.
                return path::under(CanonicalPath(path::parent_path(source)), root) &&
                       !llvm::is_contained(paths, source);
            }));
        std::ranges::sort(stable, {}, [](const std::string& source) {
            return std::tuple(llvm::count_if(source, [](char c) { return path::is_separator(c); }),
                              llvm::StringRef(source));
        });
        auto duplicates = std::ranges::unique(stable);
        stable.erase(duplicates.begin(), duplicates.end());
        paths.append(stable.begin(), stable.end());
        if(paths.size() > 1) {
            LOG_WARN(
                "No rule names a compilation database; the {} found apply in this order, "
                "an earlier one winning for a file both list: {}. To switch between them "
                "instead, declare each on a tagged rule: [[rules]] configuration = \"...\" "
                "compile_commands = [\"...\"]",
                paths.size(),
                llvm::join(paths, ", "));
        }
    }
    for(auto& path: paths) {
        auto id = project.cdb.add_source(path);
        if(auto loaded = project.cdb.load_source(id)) {
            LOG_INFO("Loaded CDB from {} with {} entries", project.cdb.source_path(id), *loaded);
            entries += *loaded;
        } else {
            LOG_WARN("Compilation database {} is not readable yet", project.cdb.source_path(id));
        }
    }
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", entries, cdb_timer.ms());

    load.members = project.build.members();
    if(load.members.empty()) {
        return load;
    }

    auto scan =
        scan_dependency_graph(project.cdb, project.dep_graph, project.build.units(load.members));
    project.dep_graph.build_reverse_map();

    auto unresolved = scan.includes_found - scan.includes_resolved;
    double accuracy =
        scan.includes_found > 0
            ? 100.0 * static_cast<double>(scan.includes_resolved) / scan.includes_found
            : 100.0;
    LOG_INFO(
        "Dependency scan: {}ms, {} files ({} source + {} header), "
        "{} edges, {}/{} resolved ({:.1f}%), {} waves",
        scan.elapsed_ms,
        scan.total_files,
        scan.source_files,
        scan.header_files,
        scan.total_edges,
        scan.includes_resolved,
        scan.includes_found,
        accuracy,
        scan.waves);
    if(unresolved > 0)
        LOG_WARN("{} unresolved includes", unresolved);
    LOG_PERF("startup",
             "phase=dep_scan files={} edges={} elapsed_ms={}",
             scan.total_files,
             scan.total_edges,
             scan.elapsed_ms);
    return load;
}

}  // namespace clice
