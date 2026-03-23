#include "syntax/dependency_graph.h"

#include <chrono>

#include "compile/toolchain.h"
#include "eventide/async/async.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"

namespace clice {

namespace et = eventide;

// ============================================================================
// DependencyGraph implementation
// ============================================================================

void DependencyGraph::add_module(llvm::StringRef module_name, std::uint32_t path_id) {
    auto [it, inserted] = module_to_path.try_emplace(module_name, path_id);
    if(!inserted && it->second != path_id) {
        LOG_WARN("Duplicate module '{}': PathID {} overwrites {}",
                 module_name,
                 path_id,
                 it->second);
        it->second = path_id;
    }
}

std::optional<std::uint32_t> DependencyGraph::lookup_module(llvm::StringRef module_name) const {
    auto it = module_to_path.find(module_name);
    if(it != module_to_path.end()) {
        return it->second;
    }
    return std::nullopt;
}

void DependencyGraph::set_includes(std::uint32_t path_id,
                                   std::uint32_t config_id,
                                   llvm::SmallVector<std::uint32_t> included_ids) {
    IncludeKey key{path_id, config_id};
    includes[key] = std::move(included_ids);
    file_configs[path_id].push_back(config_id);
}

llvm::ArrayRef<std::uint32_t> DependencyGraph::get_includes(std::uint32_t path_id,
                                                            std::uint32_t config_id) const {
    auto it = includes.find(IncludeKey{path_id, config_id});
    if(it != includes.end()) {
        return it->second;
    }
    return {};
}

llvm::SmallVector<std::uint32_t> DependencyGraph::get_all_includes(std::uint32_t path_id) const {
    llvm::DenseSet<std::uint32_t> seen;
    llvm::SmallVector<std::uint32_t> result;

    auto fc_it = file_configs.find(path_id);
    if(fc_it == file_configs.end()) {
        return result;
    }

    for(auto config_id: fc_it->second) {
        auto it = includes.find(IncludeKey{path_id, config_id});
        if(it != includes.end()) {
            for(auto id: it->second) {
                auto raw_id = id & PATH_ID_MASK;
                if(seen.insert(raw_id).second) {
                    result.push_back(id);
                }
            }
        }
    }
    return result;
}

std::size_t DependencyGraph::file_count() const {
    return file_configs.size();
}

std::size_t DependencyGraph::module_count() const {
    return module_to_path.size();
}

std::size_t DependencyGraph::edge_count() const {
    std::size_t count = 0;
    for(auto& [key, ids]: includes) {
        count += ids.size();
    }
    return count;
}

// ============================================================================
// Wavefront BFS scanner — async implementation
// ============================================================================

namespace {

struct WaveEntry {
    std::uint32_t path_id;
    std::uint32_t config_id;
};

/// Result of scanning a single file (returned from worker thread).
struct FileScanResult {
    std::string path;
    std::uint32_t path_id;
    std::uint32_t config_id;
    ScanResult scan_result;
    bool read_failed = false;
    std::int64_t read_us = 0;
    std::int64_t scan_us = 0;
};

/// Result of resolving includes for a single file (on event loop thread).
struct FileResolveResult {
    std::uint32_t path_id;
    std::uint32_t config_id;
    std::string module_name;
    bool is_interface_unit = false;
    std::size_t total_includes = 0;

    struct IncludeEdge {
        std::string resolved_path;
        unsigned found_dir_idx;
        bool conditional;
    };

    struct UnresolvedEdge {
        std::string header;
        bool is_angled;
        bool conditional;
    };

    std::vector<IncludeEdge> edges;
    std::vector<UnresolvedEdge> unresolved;

    /// Stat counters accumulated during include resolution.
    StatCounters stat_counters;
};

/// Scan a single file: read content + lexer scan.
/// Runs on libuv worker thread via queue().
FileScanResult scan_file_worker(std::string path, std::uint32_t path_id, std::uint32_t config_id) {
    FileScanResult result;
    result.path = std::move(path);
    result.path_id = path_id;
    result.config_id = config_id;

    auto t0 = std::chrono::steady_clock::now();
    auto content = et::fs::sync::read_to_string(result.path);
    auto t1 = std::chrono::steady_clock::now();
    result.read_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    if(!content.has_value()) {
        result.read_failed = true;
        return result;
    }

    result.scan_result = scan(content.value());
    auto t2 = std::chrono::steady_clock::now();
    result.scan_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

    return result;
}

/// Resolve all includes for a scanned file.
FileResolveResult resolve_file_includes(FileScanResult scan_result,
                                        const SearchConfig& config,
                                        DirListingCache& dir_cache) {
    FileResolveResult result;
    result.path_id = scan_result.path_id;
    result.config_id = scan_result.config_id;
    result.module_name = std::move(scan_result.scan_result.module_name);
    result.is_interface_unit = scan_result.scan_result.is_interface_unit;

    auto includer_dir = llvm::sys::path::parent_path(scan_result.path);
    result.total_includes = scan_result.scan_result.includes.size();

    for(auto& inc: scan_result.scan_result.includes) {
        auto resolved = resolve_include(inc.path,
                                        inc.is_angled,
                                        includer_dir,
                                        inc.is_include_next,
                                        0,  // default found_dir_idx
                                        config,
                                        dir_cache,
                                        &result.stat_counters);
        if(!resolved.has_value()) {
            result.unresolved.push_back({inc.path, inc.is_angled, inc.conditional});
            continue;
        }

        result.edges.push_back({
            std::move(resolved->path),
            resolved->found_dir_idx,
            inc.conditional,
        });
    }

    return result;
}

/// The async scan implementation that runs on a local event loop.
et::task<> scan_impl(CompilationDatabase& cdb,
                     const std::vector<UpdateInfo>& updates,
                     PathPool& path_pool,
                     DependencyGraph& graph,
                     ScanReport& report,
                     et::event_loop& loop) {
    auto start_time = std::chrono::steady_clock::now();

    // Group files by context pointer to identify unique compilation commands.
    // Convert CDB string IDs to PathPool IDs.
    llvm::DenseMap<const void*, llvm::SmallVector<std::uint32_t>> context_groups;
    llvm::DenseMap<const void*, std::uint32_t> context_to_config_id;

    for(auto& update: updates) {
        if(update.kind == UpdateKind::Deleted) {
            continue;
        }
        auto path = cdb.resolve_path(update.path_id);
        auto pool_id = path_pool.intern(path);
        context_groups[update.context].push_back(pool_id);
    }

    // Extract SearchConfig for each unique context.
    llvm::DenseMap<std::uint32_t, SearchConfig> configs;
    std::uint32_t next_config_id = 0;

    auto config_start = std::chrono::steady_clock::now();

    // Pre-warm toolchain cache: extract unique queries, execute in parallel.
    {
        std::vector<std::pair<llvm::StringRef, const void*>> file_contexts;
        for(auto& [context, file_ids]: context_groups) {
            auto representative_path = path_pool.resolve(file_ids[0]);
            file_contexts.push_back({representative_path, context});
        }

        auto pending = cdb.get_pending_toolchain_queries(file_contexts);
        if(!pending.empty()) {
            LOG_INFO("Warming toolchain cache: {} unique queries", pending.size());

            std::vector<et::task<CompilationDatabase::ToolchainResult, et::error>> tasks;
            tasks.reserve(pending.size());
            for(auto& query: pending) {
                tasks.push_back(et::queue(
                    [q = std::move(query)]() -> CompilationDatabase::ToolchainResult {
                        CompilationDatabase::ToolchainResult result;
                        result.key = q.key;
                        // Use a local allocator for the callback — query_toolchain
                        // stores returned pointers but we only need the owned strings.
                        llvm::BumpPtrAllocator alloc;
                        llvm::StringSaver saver(alloc);
                        toolchain::query_toolchain(
                            {q.file,
                             q.directory,
                             q.query_args,
                             [&](const char* s) -> const char* {
                                 result.cc1_args.push_back(s);
                                 return saver.save(s).data();
                             }});
                        return result;
                    },
                    loop));
            }

            auto outcome = co_await et::when_all(std::move(tasks));
            if(outcome.has_value()) {
                cdb.inject_toolchain_results(*outcome);
            } else {
                LOG_ERROR("Parallel toolchain query failed: {}", outcome.error().message());
            }
        }
    }

    for(auto& [context, file_ids]: context_groups) {
        std::uint32_t config_id = next_config_id++;
        context_to_config_id[context] = config_id;

        // All toolchain queries should be cached now; lookup is fast.
        auto representative_path = path_pool.resolve(file_ids[0]);
        auto ctx = cdb.lookup(representative_path,
                              {.resource_dir = true, .query_toolchain = true},
                              context);
        configs[config_id] = cdb.extract_search_config(ctx);
    }

    auto config_end = std::chrono::steady_clock::now();
    report.config_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(config_end - config_start).count();
    LOG_INFO("Extracted {} configs in {}ms ({} context groups, toolchain pre-warmed)",
             configs.size(),
             report.config_ms,
             context_groups.size());

    // Shared directory listing cache for include resolution.
    DirListingCache dir_cache;

    // Track which files have been scanned (by absolute path).
    llvm::StringMap<std::uint32_t> scanned_files;

    // Wave 0: all source files from CDB.
    std::vector<WaveEntry> current_wave;

    for(auto& [context, file_ids]: context_groups) {
        auto config_id = context_to_config_id[context];
        for(auto path_id: file_ids) {
            auto path = path_pool.resolve(path_id);
            scanned_files.try_emplace(path, path_id);
            current_wave.push_back({path_id, config_id});
        }
    }

    report.source_files = current_wave.size();
    std::size_t wave_num = 0;

    while(!current_wave.empty()) {
        auto wave_start = std::chrono::steady_clock::now();

        // Phase 1: Read + scan all files in parallel on the thread pool.
        std::vector<et::task<FileScanResult, et::error>> scan_tasks;
        scan_tasks.reserve(current_wave.size());
        for(auto& entry: current_wave) {
            auto path = std::string(path_pool.resolve(entry.path_id));
            auto pid = entry.path_id;
            auto cid = entry.config_id;
            scan_tasks.push_back(et::queue(
                [path = std::move(path), pid, cid]() {
                    return scan_file_worker(std::string(path), pid, cid);
                },
                loop));
        }

        auto scan_outcome = co_await et::when_all(std::move(scan_tasks));
        if(scan_outcome.has_error()) {
            LOG_ERROR("Parallel scan failed: {}", scan_outcome.error().message());
            break;
        }
        auto& scan_results = *scan_outcome;

        auto phase1_end = std::chrono::steady_clock::now();

        // Accumulate per-file read/scan timing into report.
        for(auto& sr: scan_results) {
            report.read_us += sr.read_us;
            report.scan_us += sr.scan_us;
        }

        // Phase 2: Resolve includes on main thread.
        // Parallelizing this doesn't help — the thread pool contention
        // slows down Phase 1 more than Phase 2 gains.
        std::vector<FileResolveResult> resolve_results;

        for(auto& scan_result: scan_results) {
            if(scan_result.read_failed) {
                LOG_WARN("Failed to read file for scanning: {}", scan_result.path);
                continue;
            }

            report.total_files++;

            auto config_it = configs.find(scan_result.config_id);
            if(config_it == configs.end()) {
                continue;
            }

            resolve_results.push_back(
                resolve_file_includes(std::move(scan_result), config_it->second, dir_cache));
        }

        auto phase2_end = std::chrono::steady_clock::now();

        // Phase 3: Process results on main thread — intern paths, build graph,
        // collect next wave.
        std::vector<WaveEntry> next_wave;

        for(auto& result: resolve_results) {
            report.includes_found += result.total_includes;
            report.includes_resolved += result.edges.size();
            report.dir_listings += result.stat_counters.dir_listings;
            report.dir_hits += result.stat_counters.dir_hits;
            report.fs_lookups += result.stat_counters.lookups;
            report.fs_us += result.stat_counters.us;

            // Record module mapping.
            if(!result.module_name.empty()) {
                graph.add_module(result.module_name, result.path_id);
            }

            // Collect unresolved includes.
            for(auto& u: result.unresolved) {
                report.unresolved.push_back({
                    std::move(u.header),
                    std::string(path_pool.resolve(result.path_id)),
                    u.is_angled,
                    u.conditional,
                });
            }

            // Build include edge list and discover new files.
            llvm::SmallVector<std::uint32_t> include_ids;

            for(auto& edge: result.edges) {
                auto inc_path_id = path_pool.intern(edge.resolved_path);

                std::uint32_t flagged_id = inc_path_id;
                if(edge.conditional) {
                    flagged_id |= DependencyGraph::CONDITIONAL_FLAG;
                    report.conditional_edges++;
                } else {
                    report.unconditional_edges++;
                }
                report.total_edges++;
                include_ids.push_back(flagged_id);

                // If this is a newly discovered file, add to next wave.
                auto [it, inserted] = scanned_files.try_emplace(edge.resolved_path, inc_path_id);
                if(inserted) {
                    next_wave.push_back({inc_path_id, result.config_id});
                }
            }

            graph.set_includes(result.path_id, result.config_id, std::move(include_ids));
        }

        auto phase3_end = std::chrono::steady_clock::now();

        auto p1 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase1_end - wave_start).count();
        auto p2 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase2_end - phase1_end).count();
        auto p3 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase3_end - phase2_end).count();

        report.phase1_ms += p1;
        report.phase2_ms += p2;
        report.phase3_ms += p3;

        LOG_INFO("Wave {}: {} files | read+scan={}ms resolve={}ms graph={}ms | next={}",
                 wave_num,
                 current_wave.size(),
                 p1,
                 p2,
                 p3,
                 next_wave.size());

        current_wave = std::move(next_wave);
        wave_num++;
    }

    auto end_time = std::chrono::steady_clock::now();
    report.elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    report.header_files = report.total_files - report.source_files;
    report.modules = graph.module_count();
    report.waves = wave_num;
}

}  // namespace

// ============================================================================
// Public sync entry point
// ============================================================================

ScanReport scan_dependency_graph(CompilationDatabase& cdb,
                                 const std::vector<UpdateInfo>& updates,
                                 PathPool& path_pool,
                                 DependencyGraph& graph) {
    ScanReport report;
    if(updates.empty()) {
        return report;
    }

    et::event_loop loop;
    loop.schedule(scan_impl(cdb, updates, path_pool, graph, report, loop));
    loop.run();
    return report;
}

}  // namespace clice
