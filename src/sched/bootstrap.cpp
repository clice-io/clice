#include "sched/bootstrap.h"

#include <string>
#include <vector>

#include "index/database.h"
#include "sched/index/pump.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/dependency_graph.h"

namespace clice {

BootstrapReport bootstrap_workspace(Workspace& workspace,
                                    IndexStore& store,
                                    IndexPump& pump,
                                    llvm::StringRef root,
                                    bool read_only_index) {
    BootstrapReport report;
    auto& cfg = workspace.config.project;

    if(!workspace.store && !cfg.cache_dir.empty()) {
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
            cache->register_namespace({.name = std::string(header_context_ns),
                                       .extension = ".h",
                                       .policy = CachePolicy::LRU,
                                       .max_bytes = 1 * GiB});
            // Synthesized header-context files once lived directly under
            // the cache root, outside the store. A directory an earlier
            // version left there is regenerable, and its files would
            // otherwise stay navigable through the metadata that names
            // them.
            fs::remove_all(path::join(cfg.cache_dir, header_context_ns));
            workspace.store.emplace(std::move(*cache));
            // A read-only bootstrap opens the index database read-only:
            // no writer lock (a concurrent server or index run keeps
            // owning it), while the persisted version stamps and artifact
            // metadata still seed this session's fast paths. Its own
            // metadata stays in memory and exits with it.
            workspace.index_db = index::open_database(*workspace.store, read_only_index);
            if(!read_only_index) {
                // The artifact metadata moved into the index database; a
                // cache.json left in the store by an older clice would sit
                // there forever.
                fs::remove(path::join(workspace.store->base_dir(), "cache.json"));
            }
            LOG_INFO("Cache store: {}", workspace.store->base_dir());
            report.opened_store = true;
        }
    }

    auto load = load_build(workspace, root);
    report.has_commands = !load.members.empty() || workspace.config.declares_sources();
    report.members = std::move(load.members);
    // Persisted index shards are CDB-independent; they load even with no
    // member yet, so a database generated later (picked up by the CDB
    // poll) starts from the previous session's index.
    pump.claim_report(store.load(read_only_index).report);

    if(cfg.enable_indexing.value && !report.members.empty()) {
        for(auto member: report.members) {
            // Bulk sweep of unknown staleness: the hash gate decides per
            // file. DepsOnly — a cold start with a warm index cache must
            // keep serving the loaded shards, not blank every query until
            // the sweep drains.
            pump.enqueue(member, ReindexReason::DepsOnly);
        }
        pump.schedule();
    }
    return report;
}

BuildLoad load_build(Workspace& workspace, llvm::StringRef root) {
    BuildLoad load;
    workspace.cdb.set_workspace_root(root);
    workspace.build.reset_active();

    ScopedTimer cdb_timer;
    std::size_t entries = 0;
    llvm::SmallVector<std::string> paths;
    for(auto declared: workspace.build.declared_sources()) {
        paths.push_back(declared.str());
    }
    if(!workspace.config.declares_sources()) {
        auto found = discover_compile_commands(root);
        if(!found.empty()) {
            paths.push_back(found);
        }
    }
    for(auto& path: paths) {
        auto id = workspace.cdb.add_source(path);
        if(auto loaded = workspace.cdb.load_source(id)) {
            LOG_INFO("Loaded CDB from {} with {} entries", workspace.cdb.source_path(id), *loaded);
            entries += *loaded;
        } else {
            LOG_WARN("Compilation database {} is not readable yet", workspace.cdb.source_path(id));
        }
    }
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", entries, cdb_timer.ms());

    load.members = workspace.build.members();
    if(load.members.empty()) {
        return load;
    }

    auto scan = scan_dependency_graph(workspace.cdb,
                                      workspace.dep_graph,
                                      workspace.build.units(load.members));
    workspace.dep_graph.build_reverse_map();

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
