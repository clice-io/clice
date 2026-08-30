#include "sched/bootstrap.h"

#include <string>
#include <vector>

#include "index/database.h"
#include "sched/context.h"
#include "sched/index/pump.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "support/cache_store.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/dependency_graph.h"

namespace clice {

BootstrapReport bootstrap_workspace(Workspace& workspace,
                                    ContextResolver& contexts,
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
            cache->register_namespace(
                {.name = "header_context", .extension = ".h", .policy = CachePolicy::Scratch});
            workspace.store.emplace(std::move(*cache));
            // A read-only bootstrap never opens the index database: it
            // would hold the writer lock for the process lifetime while
            // committing nothing, starving a concurrent server or index
            // run. The store's entry points all tolerate its absence.
            if(!read_only_index) {
                workspace.index_db = index::open_database(*workspace.store, cfg.index_db);
            }
            LOG_INFO("Cache store: {}", workspace.store->base_dir());

            workspace.load_cache(contexts);
            report.opened_store = true;
        }
    }

    workspace.cdb.set_workspace_root(root);
    report.cdb_path = discover_compile_commands(workspace.config, root);
    if(report.cdb_path.empty()) {
        // Persisted index shards are CDB-independent; load them so a
        // database generated later (picked up by the CDB poll) starts from
        // the previous session's index.
        pump.claim_report(store.load(read_only_index).report);
        return report;
    }

    ScopedTimer cdb_timer;
    auto count = workspace.cdb.load(report.cdb_path).value_or(0);
    LOG_INFO("Loaded CDB from {} with {} entries", report.cdb_path, count);
    LOG_PERF("startup", "phase=cdb_load entries={} elapsed_ms={}", count, cdb_timer.ms());

    auto scan = scan_dependency_graph(workspace.cdb,
                                      workspace.dep_graph,
                                      /*cache=*/nullptr,
                                      [&workspace](llvm::StringRef path,
                                                   std::vector<std::string>& append,
                                                   std::vector<std::string>& remove) {
                                          workspace.config.match_rules(path, append, remove);
                                      });
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

    workspace.build_module_map();
    pump.claim_report(store.load(read_only_index).report);

    if(cfg.enable_indexing.value) {
        for(auto& entry: workspace.cdb.entries()) {
            // Bulk sweep of unknown staleness: the hash gate decides per
            // file. DepsOnly — a cold start with a warm index cache must
            // keep serving the loaded shards, not blank every query until
            // the sweep drains.
            pump.enqueue(entry.file, ReindexReason::DepsOnly);
        }
        pump.schedule();
    }
    return report;
}

}  // namespace clice
