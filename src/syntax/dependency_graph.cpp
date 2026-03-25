#include "syntax/dependency_graph.h"

#include <chrono>

#include "compile/toolchain.h"
#include "eventide/async/async.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
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

/// Result of scanning a single file (returned from worker thread).
struct FileScanResult {
    const char* path;  // Stable pointer from PathPool.
    std::uint32_t path_id;
    std::uint32_t config_id;
    ScanResult scan_result;
    bool read_failed = false;
    std::int64_t read_us = 0;
    std::int64_t scan_us = 0;
};

/// Scan a single file: read content + lexer scan.
/// Runs on libuv worker thread via queue().
/// @param path  Stable pointer from PathPool (must outlive the task).
FileScanResult scan_file_worker(const char* path, std::uint32_t path_id, std::uint32_t config_id) {
    FileScanResult result;
    result.path = path;
    result.path_id = path_id;
    result.config_id = config_id;

    auto t0 = std::chrono::steady_clock::now();
    // Force read() instead of mmap: RequiresNullTerminator=true makes LLVM
    // fall back to read() for page-aligned files, and IsVolatile=true forces
    // read() unconditionally — bypassing mmap entirely.  This separates
    // actual I/O cost from page-fault cost that was previously hidden inside
    // the lexer timing.
    auto buf = llvm::MemoryBuffer::getFile(result.path,
                                           /*FileSize=*/-1,
                                           /*RequiresNullTerminator=*/true,
                                           /*IsVolatile=*/true);
    auto t1 = std::chrono::steady_clock::now();
    result.read_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    if(!buf) {
        result.read_failed = true;
        return result;
    }

    result.scan_result = scan((*buf)->getBuffer());
    auto t2 = std::chrono::steady_clock::now();
    result.scan_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

    return result;
}

/// The async scan implementation that runs on a local event loop.
et::task<> scan_impl(CompilationDatabase& cdb,
                     const std::vector<UpdateInfo>& updates,
                     PathPool& path_pool,
                     DependencyGraph& graph,
                     ScanReport& report,
                     ScanCache* ext_cache,
                     et::event_loop& loop) {
    auto start_time = std::chrono::steady_clock::now();

    // Reuse context groups and configs from cache when available (warm runs).
    // On the first call (or when cache is null) we build everything from scratch.
    const bool have_config_cache =
        ext_cache && !ext_cache->context_groups.empty() && !ext_cache->configs.empty();

    // Provide local storage when not using the persistent cache.
    llvm::DenseMap<const void*, llvm::SmallVector<std::uint32_t>> local_context_groups;
    llvm::DenseMap<const void*, std::uint32_t> local_context_to_config_id;
    llvm::DenseMap<std::uint32_t, SearchConfig> local_configs;

    // When ext_cache is provided, write directly into it so that the data
    // survives across calls (making have_config_cache true on run 2+).
    llvm::DenseMap<const void*, llvm::SmallVector<std::uint32_t>>& context_groups =
        ext_cache ? ext_cache->context_groups : local_context_groups;
    llvm::DenseMap<const void*, std::uint32_t>& context_to_config_id =
        ext_cache ? ext_cache->context_to_config_id : local_context_to_config_id;
    llvm::DenseMap<std::uint32_t, SearchConfig>& configs =
        ext_cache ? ext_cache->configs : local_configs;

    auto config_start = std::chrono::steady_clock::now();

    if(!have_config_cache) {
        // Group files by context pointer to identify unique compilation commands.
        // Convert CDB string IDs to PathPool IDs.
        for(auto& update: updates) {
            if(update.kind == UpdateKind::Deleted) {
                continue;
            }
            auto path = cdb.resolve_path(update.path_id);
            auto pool_id = path_pool.intern(path);
            context_groups[update.context].push_back(pool_id);
        }

        // Pre-warm toolchain cache: extract unique queries, execute in parallel.
        // Skip entirely when configs are already cached (warm runs), since the
        // toolchain cache is necessarily also populated from the previous scan.
        auto prewarm_start = std::chrono::steady_clock::now();
        if(!cdb.has_cached_configs()) {
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
                            llvm::BumpPtrAllocator alloc;
                            llvm::StringSaver saver(alloc);
                            toolchain::query_toolchain({q.file,
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
        auto prewarm_end = std::chrono::steady_clock::now();
        report.prewarm_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(prewarm_end - prewarm_start)
                .count();

        // Extract SearchConfig for each unique context.
        std::uint32_t next_config_id = 0;
        std::int64_t lookup_us = 0;
        for(auto& [context, file_ids]: context_groups) {
            std::uint32_t config_id = next_config_id++;
            context_to_config_id[context] = config_id;
            auto representative_path = path_pool.resolve(file_ids[0]);
            auto t0 = std::chrono::steady_clock::now();
            configs[config_id] =
                cdb.lookup_search_config(representative_path,
                                         {.resource_dir = true, .query_toolchain = true},
                                         context);
            auto t1 = std::chrono::steady_clock::now();
            lookup_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        }
        report.config_loop_ms = lookup_us / 1000;
        LOG_INFO("Config extracted: {} groups, {:.1f}ms", configs.size(), lookup_us / 1000.0);
    }

    auto config_end = std::chrono::steady_clock::now();
    report.config_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(config_end - config_start).count();

    // Use external persistent cache when provided, otherwise create a local one.
    DirListingCache local_dir_cache;
    DirListingCache& dir_cache = ext_cache ? ext_cache->dir_cache : local_dir_cache;

    llvm::StringMap<std::uint32_t> local_include_cache;
    llvm::StringMap<std::uint32_t>& include_cache =
        ext_cache ? ext_cache->include_cache : local_include_cache;

    // ── Dir cache pre-population ─────────────────────────────────────
    // Collect all unique search dirs and launch readdir tasks on the
    // thread pool.  Tasks start executing immediately but are NOT awaited
    // here — instead they run concurrently with Wave 0's file scanning
    // (Optimization 1: overlap dir cache with Phase 1).  We only await
    // them before Phase 2 of Wave 0, which is the first consumer.

    struct DirEntry {
        std::string dir_path;
        llvm::StringSet<> entries;
    };

    std::vector<et::task<DirEntry, et::error>> pending_dir_tasks;

    if(dir_cache.dirs.empty()) {
        llvm::StringSet<> unique_dirs;
        for(auto& [config_id, config]: configs) {
            for(auto& dir: config.dirs) {
                unique_dirs.insert(dir.path);
            }
        }
        // Also prefetch parent directories of source files (for quoted include resolution).
        for(auto& [context, file_ids]: context_groups) {
            for(auto path_id: file_ids) {
                auto dir = llvm::sys::path::parent_path(path_pool.resolve(path_id));
                if(!dir.empty()) {
                    unique_dirs.insert(dir);
                }
            }
        }

        pending_dir_tasks.reserve(unique_dirs.size());
        for(auto& entry: unique_dirs) {
            auto dir_path = entry.getKey().str();
            pending_dir_tasks.push_back(et::queue(
                [dir_path = std::move(dir_path)]() -> DirEntry {
                    DirEntry result;
                    result.dir_path = dir_path;
                    std::error_code ec;
                    llvm::sys::fs::directory_iterator di(result.dir_path, ec);
                    for(; !ec && di != llvm::sys::fs::directory_iterator(); di.increment(ec)) {
                        result.entries.insert(llvm::sys::path::filename(di->path()));
                    }
                    return result;
                },
                loop));
        }
        LOG_INFO("Launched {} dir cache tasks (running in background)", pending_dir_tasks.size());
    }

    // Track which files have been scanned (by path_id — cheaper than string hash).
    llvm::DenseSet<std::uint32_t> scanned_files;

    // Wave 0: all source files from CDB.
    // Re-use the cached initial_wave when available to avoid re-iterating context_groups.
    std::vector<WaveEntry> current_wave;
    const bool have_initial_wave_cache = ext_cache && !ext_cache->initial_wave.empty();
    if(have_initial_wave_cache) {
        current_wave = ext_cache->initial_wave;
        for(auto& entry: current_wave) {
            scanned_files.insert(entry.path_id);
        }
    } else {
        current_wave.reserve(updates.size());
        for(auto& [context, file_ids]: context_groups) {
            auto config_id = context_to_config_id[context];
            for(auto path_id: file_ids) {
                scanned_files.insert(path_id);
                current_wave.push_back({path_id, config_id});
            }
        }
        if(ext_cache) {
            ext_cache->initial_wave = current_wave;
        }
    }

    report.source_files = current_wave.size();
    std::size_t wave_num = 0;

    // Optimization 2: prefetch scan tasks.
    // During Phase 2 of wave N, newly discovered files are immediately
    // queued for scanning on the thread pool.  When wave N+1 starts,
    // these tasks are already running (or finished), eliminating most
    // of the Phase 1 wait time for subsequent waves.
    std::vector<et::task<FileScanResult, et::error>> prefetch_tasks;

    // Pre-resolved search configs: built once after dir cache is populated,
    // then reused for all waves.  Eliminates StringMap lookups in Phase 2.
    llvm::DenseMap<std::uint32_t, ResolvedSearchConfig> resolved_configs;

    while(!current_wave.empty()) {
        auto wave_start = std::chrono::steady_clock::now();

        // Phase 1: Read + scan all files in parallel on the thread pool.
        // Files with a cached ScanResult skip I/O and lexing entirely.
        // For waves > 0, files discovered during the previous wave's Phase 2
        // already have running scan tasks in prefetch_tasks.
        std::vector<FileScanResult> scan_results;
        scan_results.reserve(current_wave.size());
        std::size_t wave_cache_hits = 0;

        // Collect cache hits first (applies to all waves).
        for(auto& entry: current_wave) {
            if(ext_cache) {
                auto it = ext_cache->scan_results.find(entry.path_id);
                if(it != ext_cache->scan_results.end()) {
                    scan_results.push_back({path_pool.resolve(entry.path_id).data(),
                                            entry.path_id,
                                            entry.config_id,
                                            it->second,
                                            false,
                                            0,
                                            0});
                    report.scan_cache_hits++;
                    wave_cache_hits++;
                }
            }
        }

        if(!prefetch_tasks.empty()) {
            // Waves 1+: await prefetched scan tasks from previous Phase 2.
            auto scan_outcome = co_await et::when_all(std::move(prefetch_tasks));
            prefetch_tasks.clear();
            if(scan_outcome.has_error()) {
                LOG_ERROR("Prefetch scan failed: {}", scan_outcome.error().message());
                break;
            }
            for(auto& r: *scan_outcome) {
                if(!r.read_failed && ext_cache) {
                    ext_cache->scan_results.try_emplace(r.path_id, r.scan_result);
                }
                scan_results.push_back(std::move(r));
            }
        } else {
            // Wave 0 (or warm run with all cache hits): create scan tasks now.
            std::vector<et::task<FileScanResult, et::error>> scan_tasks;
            scan_tasks.reserve(current_wave.size());
            for(auto& entry: current_wave) {
                auto pid = entry.path_id;
                auto cid = entry.config_id;
                // Skip files already served from cache above.
                if(ext_cache && ext_cache->scan_results.count(pid)) {
                    continue;
                }
                auto path = path_pool.resolve(pid).data();
                scan_tasks.push_back(et::queue(
                    [path, pid, cid]() { return scan_file_worker(path, pid, cid); }, loop));
            }

            // Optimization 1: await dir cache tasks concurrently with scan tasks.
            // Both sets of tasks run on the same thread pool.  By awaiting dir
            // tasks first (while scan tasks continue in the background), we pay
            // max(dir_time, scan_time) instead of dir_time + scan_time.
            if(!pending_dir_tasks.empty()) {
                auto dir_t0 = std::chrono::steady_clock::now();
                auto dir_outcome = co_await et::when_all(std::move(pending_dir_tasks));
                pending_dir_tasks.clear();
                if(dir_outcome.has_value()) {
                    for(auto& entry: *dir_outcome) {
                        dir_cache.dirs.try_emplace(entry.dir_path, std::move(entry.entries));
                    }
                    LOG_INFO("Pre-populated dir cache: {} directories", dir_outcome->size());
                }
                auto dir_t1 = std::chrono::steady_clock::now();
                report.dir_cache_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(dir_t1 - dir_t0).count();
            }

            if(!scan_tasks.empty()) {
                auto scan_outcome = co_await et::when_all(std::move(scan_tasks));
                if(scan_outcome.has_error()) {
                    LOG_ERROR("Parallel scan failed: {}", scan_outcome.error().message());
                    break;
                }
                for(auto& r: *scan_outcome) {
                    if(!r.read_failed && ext_cache) {
                        ext_cache->scan_results.try_emplace(r.path_id, r.scan_result);
                    }
                    scan_results.push_back(std::move(r));
                }
            }
        }

        auto phase1_end = std::chrono::steady_clock::now();

        // Accumulate per-file read/scan timing into report.
        for(auto& sr: scan_results) {
            report.read_us += sr.read_us;
            report.scan_us += sr.scan_us;
        }

        // Pre-resolve search configs once after dir cache is populated (wave 0).
        // Converts StringMap lookups into direct pointer dereferences for Phase 2.
        if(resolved_configs.empty()) {
            for(auto& [config_id, config]: configs) {
                resolved_configs[config_id] = resolve_search_config(config, dir_cache);
            }
        }

        // Phase 2+3: Resolve includes, intern paths, build graph, collect next wave.
        // Merged into a single pass to avoid intermediate string allocations.
        // Optimization 2: newly discovered files are immediately queued for
        // scanning (prefetch_tasks), overlapping Phase 1 of the next wave
        // with Phase 2 of the current wave.
        std::vector<WaveEntry> next_wave;
        next_wave.reserve(current_wave.size());  // Heuristic: next wave ≤ current wave.
        StatCounters wave_stat_counters;

        for(auto& scan_result: scan_results) {
            if(scan_result.read_failed) {
                LOG_WARN("Failed to read file for scanning: {}", scan_result.path);
                continue;
            }

            report.total_files++;

            auto rc_it = resolved_configs.find(scan_result.config_id);
            if(rc_it == resolved_configs.end()) {
                continue;
            }

            auto& resolved_config = rc_it->second;
            auto includer_dir = llvm::sys::path::parent_path(scan_result.path);
            auto* includer_entries = resolve_dir(includer_dir, dir_cache, &wave_stat_counters);

            // Record module mapping.
            if(!scan_result.scan_result.module_name.empty()) {
                graph.add_module(scan_result.scan_result.module_name, scan_result.path_id);
            }

            report.includes_found += scan_result.scan_result.includes.size();

            llvm::SmallVector<std::uint32_t> include_ids;
            include_ids.reserve(scan_result.scan_result.includes.size());

            for(auto& inc: scan_result.scan_result.includes) {
                // For angled includes, resolution depends only on config (not includer dir).
                // Cache these to skip redundant directory searches across files.
                bool cache_eligible = inc.is_angled && !inc.is_include_next;
                llvm::SmallString<80> cache_key;
                if(cache_eligible) {
                    cache_key.append(reinterpret_cast<const char*>(&scan_result.config_id),
                                     reinterpret_cast<const char*>(&scan_result.config_id) +
                                         sizeof(std::uint32_t));
                    cache_key += inc.path;

                    auto cache_it = include_cache.find(cache_key);
                    if(cache_it != include_cache.end()) {
                        report.include_cache_hits++;
                        auto cached_id = cache_it->second;
                        if(cached_id == UINT32_MAX) {
                            report.unresolved.push_back({
                                std::move(inc.path),
                                std::string(path_pool.resolve(scan_result.path_id)),
                                inc.is_angled,
                                inc.conditional,
                            });
                            continue;
                        }
                        report.includes_resolved++;
                        // Jump directly to edge building with cached path_id.
                        std::uint32_t flagged_id = cached_id;
                        if(inc.conditional) {
                            flagged_id |= DependencyGraph::CONDITIONAL_FLAG;
                            report.conditional_edges++;
                        } else {
                            report.unconditional_edges++;
                        }
                        report.total_edges++;
                        include_ids.push_back(flagged_id);
                        if(scanned_files.insert(cached_id).second) {
                            next_wave.push_back({cached_id, scan_result.config_id});
                        }
                        continue;
                    }
                }

                auto r_t0 = std::chrono::steady_clock::now();
                auto resolved = resolve_include(inc.path,
                                                inc.is_angled,
                                                includer_entries,
                                                includer_dir,
                                                inc.is_include_next,
                                                0,
                                                resolved_config,
                                                dir_cache,
                                                &wave_stat_counters);
                auto r_t1 = std::chrono::steady_clock::now();
                report.p2_resolve_us +=
                    std::chrono::duration_cast<std::chrono::microseconds>(r_t1 - r_t0).count();
                if(!resolved.has_value()) {
                    if(cache_eligible) {
                        include_cache.try_emplace(cache_key, UINT32_MAX);
                    }
                    report.unresolved.push_back({
                        std::move(inc.path),
                        std::string(path_pool.resolve(scan_result.path_id)),
                        inc.is_angled,
                        inc.conditional,
                    });
                    continue;
                }

                auto inc_path_id = path_pool.intern(resolved->path);
                report.includes_resolved++;

                if(cache_eligible) {
                    include_cache.try_emplace(cache_key, inc_path_id);
                }

                std::uint32_t flagged_id = inc_path_id;
                if(inc.conditional) {
                    flagged_id |= DependencyGraph::CONDITIONAL_FLAG;
                    report.conditional_edges++;
                } else {
                    report.unconditional_edges++;
                }
                report.total_edges++;
                include_ids.push_back(flagged_id);

                if(scanned_files.insert(inc_path_id).second) {
                    next_wave.push_back({inc_path_id, scan_result.config_id});
                    // Prefetch: start scanning this file immediately on the
                    // thread pool so it's ready when the next wave begins.
                    if(!ext_cache ||
                       ext_cache->scan_results.find(inc_path_id) ==
                           ext_cache->scan_results.end()) {
                        auto inc_path = path_pool.resolve(inc_path_id).data();
                        prefetch_tasks.push_back(et::queue(
                            [inc_path, inc_path_id, cid = scan_result.config_id]() {
                                return scan_file_worker(inc_path, inc_path_id, cid);
                            },
                            loop));
                    }
                }
            }

            graph.set_includes(scan_result.path_id, scan_result.config_id, std::move(include_ids));
        }

        report.dir_listings += wave_stat_counters.dir_listings;
        report.dir_hits += wave_stat_counters.dir_hits;
        report.fs_lookups += wave_stat_counters.lookups;
        report.fs_us += wave_stat_counters.us;

        auto phase2_end = std::chrono::steady_clock::now();
        auto phase3_end = phase2_end;

        auto p1 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase1_end - wave_start).count();
        auto p2 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase2_end - phase1_end).count();
        auto p3 =
            std::chrono::duration_cast<std::chrono::milliseconds>(phase3_end - phase2_end).count();

        report.phase1_ms += p1;
        report.phase2_ms += p2;
        report.phase3_ms += p3;

        // Record per-wave stats for cold start analysis.
        ScanReport::WaveStats ws;
        ws.files = current_wave.size();
        ws.phase1_ms = p1;
        ws.phase2_ms = p2;
        ws.next_files = next_wave.size();
        ws.prefetch_count = prefetch_tasks.size();
        ws.dir_listings = wave_stat_counters.dir_listings;
        ws.dir_hits = wave_stat_counters.dir_hits;
        ws.cache_hits = wave_cache_hits;
        report.wave_stats.push_back(ws);

        LOG_INFO("Wave {}: {} files | read+scan={}ms resolve={}ms graph={}ms | next={} "
                 "prefetch={}",
                 wave_num,
                 current_wave.size(),
                 p1,
                 p2,
                 p3,
                 next_wave.size(),
                 prefetch_tasks.size());

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
                                 DependencyGraph& graph,
                                 ScanCache* cache) {
    ScanReport report;
    if(updates.empty()) {
        return report;
    }

    et::event_loop loop;
    loop.schedule(scan_impl(cdb, updates, path_pool, graph, report, cache, loop));
    loop.run();
    return report;
}

}  // namespace clice
