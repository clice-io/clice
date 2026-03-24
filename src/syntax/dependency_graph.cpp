#include "syntax/dependency_graph.h"

#include <chrono>

#include "compile/toolchain.h"
#include "eventide/async/async.h"
#include "support/logging.h"
#include "syntax/include_resolver.h"
#include "syntax/scan.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/ADT/StringSet.h"
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

/// Scan a single file: read content + lexer scan.
/// Runs on libuv worker thread via queue().
FileScanResult scan_file_worker(std::string path, std::uint32_t path_id, std::uint32_t config_id) {
    FileScanResult result;
    result.path = std::move(path);
    result.path_id = path_id;
    result.config_id = config_id;

    auto t0 = std::chrono::steady_clock::now();
    auto buf = llvm::MemoryBuffer::getFile(result.path,
                                               /*FileSize=*/-1,
                                               /*RequiresNullTerminator=*/false,
                                               /*IsVolatile=*/false);
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
                            {q.file, q.directory, q.query_args, [&](const char* s) -> const char* {
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

    std::int64_t lookup_us = 0;
    std::int64_t extract_us = 0;
    std::size_t config_count = 0;

    for(auto& [context, file_ids]: context_groups) {
        std::uint32_t config_id = next_config_id++;
        context_to_config_id[context] = config_id;

        auto representative_path = path_pool.resolve(file_ids[0]);

        auto t0 = std::chrono::steady_clock::now();
        auto ctx = cdb.lookup(representative_path,
                              {.resource_dir = true, .query_toolchain = true},
                              context);
        auto t1 = std::chrono::steady_clock::now();
        configs[config_id] = cdb.extract_search_config(ctx);
        auto t2 = std::chrono::steady_clock::now();

        lookup_us += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        extract_us += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        config_count++;
    }

    auto config_end = std::chrono::steady_clock::now();
    report.prewarm_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(prewarm_end - config_start).count();
    report.config_loop_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(config_end - prewarm_end).count();
    report.config_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(config_end - config_start).count();
    LOG_INFO(
        "Config: {}ms total (prewarm={}ms, loop={}ms [{} groups, " "lookup={:.1f}ms, extract={:.1f}ms])",
        report.config_ms,
        report.prewarm_ms,
        report.config_loop_ms,
        config_count,
        lookup_us / 1000.0,
        extract_us / 1000.0);

    // Shared directory listing cache for include resolution.
    DirListingCache dir_cache;

    // Pre-populate dir cache: collect all unique search dirs and list them
    // in parallel on the thread pool. This avoids serial readdir() syscalls
    // during Phase 2 of the first wave (especially impactful on Windows).
    {
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

        struct DirEntry {
            std::string dir_path;
            llvm::StringSet<> entries;
        };

        std::vector<et::task<DirEntry, et::error>> dir_tasks;
        dir_tasks.reserve(unique_dirs.size());
        for(auto& entry: unique_dirs) {
            auto dir_path = entry.getKey().str();
            dir_tasks.push_back(et::queue(
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

        auto dir_outcome = co_await et::when_all(std::move(dir_tasks));
        if(dir_outcome.has_value()) {
            for(auto& entry: *dir_outcome) {
                dir_cache.dirs.try_emplace(entry.dir_path, std::move(entry.entries));
            }
            LOG_INFO("Pre-populated dir cache: {} directories", dir_outcome->size());
        }
    }

    // Track which files have been scanned (by path_id — cheaper than string hash).
    llvm::DenseSet<std::uint32_t> scanned_files;

    // Wave 0: all source files from CDB.
    std::vector<WaveEntry> current_wave;
    current_wave.reserve(updates.size());

    for(auto& [context, file_ids]: context_groups) {
        auto config_id = context_to_config_id[context];
        for(auto path_id: file_ids) {
            scanned_files.insert(path_id);
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
            auto pid = entry.path_id;
            auto cid = entry.config_id;
            scan_tasks.push_back(et::queue(
                [path = std::string(path_pool.resolve(pid)), pid, cid]() mutable {
                    return scan_file_worker(std::move(path), pid, cid);
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

        // Phase 2+3: Resolve includes, intern paths, build graph, collect next wave.
        // Merged into a single pass to avoid intermediate string allocations.
        std::vector<WaveEntry> next_wave;
        llvm::SmallString<256> candidate;
        StatCounters wave_stat_counters;

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

            auto& config = config_it->second;
            auto includer_dir = llvm::sys::path::parent_path(scan_result.path);

            // Record module mapping.
            if(!scan_result.scan_result.module_name.empty()) {
                graph.add_module(scan_result.scan_result.module_name, scan_result.path_id);
            }

            report.includes_found += scan_result.scan_result.includes.size();

            llvm::SmallVector<std::uint32_t> include_ids;
            include_ids.reserve(scan_result.scan_result.includes.size());

            for(auto& inc: scan_result.scan_result.includes) {
                auto resolved = resolve_include(inc.path,
                                                inc.is_angled,
                                                includer_dir,
                                                inc.is_include_next,
                                                0,
                                                config,
                                                dir_cache,
                                                &wave_stat_counters);
                if(!resolved.has_value()) {
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
