/// Benchmark for scan_dependency_graph on a real compilation database.
///
/// Usage:
///   scan_benchmark [OPTIONS] <compile_commands.json>
///
/// Example:
///   ./build/RelWithDebInfo/bin/scan_benchmark \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json
///
///   ./build/RelWithDebInfo/bin/scan_benchmark --log-level info --export graph.json \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <map>
#include <print>
#include <set>
#include <thread>

#include "compile/command.h"
#include "eventide/deco/deco.h"
#include "eventide/serde/json/serializer.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

#include "llvm/Support/FileSystem.h"

namespace et = eventide;

using namespace clice;

struct BenchmarkOptions {
    DecoKV(names = {"--log-level"}; help = "Log level: trace, debug, info, warn, error, off";
           required = false;)
    <std::string> log_level = "off";

    DecoKV(names = {"--export"}; help = "Export dependency graph as JSON to this path";
           required = false;)
    <std::string> export_path;

    DecoKV(names = {"--runs"}; help = "Number of cold start iterations"; required = false;)
    <int> runs = 20;

    DecoFlag(names = {"-h", "--help"}; help = "Show help message"; required = false;)
    help;

    DecoInput(meta_var = "CDB"; help = "Path to compile_commands.json"; required = false;)
    <std::string> cdb_path;
};

struct FileNode {
    std::string path;
    std::string module_name;
    std::vector<std::string> includes;
};

struct GraphExport {
    std::vector<FileNode> files;
};

void export_graph_json(const PathPool& path_pool,
                       const DependencyGraph& graph,
                       llvm::StringRef output_path) {
    // Build reverse module map: path_id -> module_name.
    llvm::DenseMap<std::uint32_t, llvm::StringRef> path_to_module;
    for(auto& [name, path_id]: graph.modules()) {
        path_to_module[path_id] = name;
    }

    GraphExport export_data;
    for(std::uint32_t id = 0; id < path_pool.paths.size(); id++) {
        auto inc_ids = graph.get_all_includes(id);
        if(inc_ids.empty()) {
            continue;
        }

        FileNode node;
        node.path = path_pool.paths[id].str();

        auto mod_it = path_to_module.find(id);
        if(mod_it != path_to_module.end()) {
            node.module_name = mod_it->second.str();
        }

        for(auto flagged_id: inc_ids) {
            auto raw_id = flagged_id & DependencyGraph::PATH_ID_MASK;
            node.includes.push_back(path_pool.paths[raw_id].str());
        }

        export_data.files.push_back(std::move(node));
    }

    auto json = et::serde::json::to_json(export_data);
    if(!json) {
        std::println(stderr, "Failed to serialize dependency graph");
        return;
    }

    std::ofstream out(output_path.str());
    out << *json;
    std::println("Graph exported to {} ({} files)", output_path, export_data.files.size());
}

void print_report(const ScanReport& report) {
    std::println("===============================================================");
    std::println("                    Dependency Scan Report");
    std::println("===============================================================");

    // Timing.
    std::println("");
    std::println("  Time: {}ms", report.elapsed_ms);
    std::println("  Waves: {}", report.waves);

    // File counts.
    std::println("");
    std::println("  Files");
    std::println("    Source files (from CDB):  {}", report.source_files);
    std::println("    Header files (discovered): {}", report.header_files);
    std::println("    Total:                     {}", report.total_files);
    std::println("    Modules:                   {}", report.modules);

    // Include edges.
    std::println("");
    std::println("  Include Edges");
    std::println("    Total:         {}", report.total_edges);
    std::println("    Unconditional: {}", report.unconditional_edges);
    std::println("    Conditional:   {} (inside #if/#ifdef)", report.conditional_edges);

    // Resolution accuracy.
    std::println("");
    std::println("  Resolution");
    std::println("    #include directives: {}", report.includes_found);
    std::println("    Resolved:            {}", report.includes_resolved);
    auto unresolved_count = report.includes_found - report.includes_resolved;
    std::println("    Unresolved:          {}", unresolved_count);
    if(report.includes_found > 0) {
        double rate = 100.0 * static_cast<double>(report.includes_resolved) /
                      static_cast<double>(report.includes_found);
        std::println("    Accuracy:            {:.1f}%", rate);
    }

    // Wall-clock phase breakdown.
    std::println("");
    std::println("  Phase Breakdown (wall-clock)");
    std::println("    Config extraction: {}ms (prewarm={}ms, loop={}ms)",
                 report.config_ms,
                 report.prewarm_ms,
                 report.config_loop_ms);
    std::println("    Dir cache pre-pop: {}ms (overlapped with Phase 1)", report.dir_cache_ms);
    std::println("    Phase 1 (read+scan, parallel): {}ms", report.phase1_ms);
    std::println("    Phase 2 (include resolve):     {}ms", report.phase2_ms);
    std::println("    Phase 3 (graph build):         {}ms", report.phase3_ms);

    // Per-wave breakdown.
    if(!report.wave_stats.empty()) {
        std::println("");
        std::println("  Per-Wave Breakdown");
        std::println("    {:>5s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>10s} {:>10s}",
                     "Wave", "Files", "P1(ms)", "P2(ms)", "Next", "Prefetch", "DirList", "DirHits");
        for(std::size_t i = 0; i < report.wave_stats.size(); i++) {
            auto& ws = report.wave_stats[i];
            std::println("    {:>5} {:>8} {:>8} {:>8} {:>8} {:>8} {:>10} {:>10}",
                         i,
                         ws.files,
                         ws.phase1_ms,
                         ws.phase2_ms,
                         ws.next_files,
                         ws.prefetch_count,
                         ws.dir_listings,
                         ws.dir_hits);
        }
    }

    // Phase 2 breakdown.
    if(report.p2_resolve_us > 0) {
        auto other_us = report.phase2_ms * 1000 - report.p2_resolve_us;
        std::println("");
        std::println("  Phase 2 Breakdown (single-threaded)");
        std::println("    resolve_include: {:.1f}ms", report.p2_resolve_us / 1000.0);
        std::println("    Other (cache lookup, intern, graph): {:.1f}ms", other_us / 1000.0);
    }

    // Cumulative I/O statistics.
    std::println("");
    std::println("  I/O Statistics (cumulative across threads)");
    std::println("    File read:  {:.1f}ms (sum of all threads)", report.read_us / 1000.0);
    std::println("    Lexer scan: {:.1f}ms (sum of all threads)", report.scan_us / 1000.0);
    std::println("    Filesystem: {:.1f}ms ({} readdir calls, {} dir cache hits)",
                 report.fs_us / 1000.0,
                 report.dir_listings,
                 report.dir_hits);
    std::println("    File lookups: {}", report.fs_lookups);
    std::println("    Include cache hits: {}", report.include_cache_hits);
    std::println("    Scan result cache hits: {}", report.scan_cache_hits);
    if(report.dir_listings + report.dir_hits > 0) {
        double hit_rate = 100.0 * static_cast<double>(report.dir_hits) /
                          static_cast<double>(report.dir_listings + report.dir_hits);
        std::println("    Dir cache hit rate: {:.1f}%", hit_rate);
    }

    // Unresolved details.
    if(!report.unresolved.empty()) {
        // Deduplicate by header name, count occurrences.
        std::map<std::string, std::size_t> unresolved_counts;
        std::map<std::string, bool> unresolved_angled;
        std::map<std::string, bool> unresolved_conditional;
        for(auto& u: report.unresolved) {
            unresolved_counts[u.header]++;
            unresolved_angled[u.header] = u.is_angled;
            if(!u.conditional) {
                unresolved_conditional[u.header] = false;
            } else if(!unresolved_conditional.contains(u.header)) {
                unresolved_conditional[u.header] = true;
            }
        }

        // Sort by count descending.
        std::vector<std::pair<std::string, std::size_t>> sorted(unresolved_counts.begin(),
                                                                unresolved_counts.end());
        std::ranges::sort(sorted, [](auto& a, auto& b) { return a.second > b.second; });

        // Split into conditional-only and unconditional.
        std::vector<std::pair<std::string, std::size_t>> unconditional_unresolved;
        std::vector<std::pair<std::string, std::size_t>> conditional_unresolved;
        for(auto& [header, count]: sorted) {
            if(unresolved_conditional[header]) {
                conditional_unresolved.push_back({header, count});
            } else {
                unconditional_unresolved.push_back({header, count});
            }
        }

        if(!unconditional_unresolved.empty()) {
            std::println("");
            std::println("  Unresolved Headers (unconditional, {} unique):",
                         unconditional_unresolved.size());
            for(auto& [header, count]: unconditional_unresolved) {
                auto bracket = unresolved_angled[header] ? '<' : '"';
                auto close = unresolved_angled[header] ? '>' : '"';
                std::println("    {}{}{} (x{})", bracket, header, close, count);
            }
        }

        if(!conditional_unresolved.empty()) {
            std::println("");
            std::println("  Unresolved Headers (conditional only, {} unique):",
                         conditional_unresolved.size());
            auto limit = std::min(conditional_unresolved.size(), std::size_t(20));
            for(std::size_t i = 0; i < limit; i++) {
                auto& [header, count] = conditional_unresolved[i];
                auto bracket = unresolved_angled[header] ? '<' : '"';
                auto close = unresolved_angled[header] ? '>' : '"';
                std::println("    {}{}{} (x{})", bracket, header, close, count);
            }
            if(conditional_unresolved.size() > limit) {
                std::println("    ... and {} more", conditional_unresolved.size() - limit);
            }
        }
    }

    std::println("");
    std::println("===============================================================");
}

int main(int argc, const char** argv) {
    auto args = deco::util::argvify(argc, argv);
    auto result = deco::cli::parse<BenchmarkOptions>(args);

    if(!result.has_value()) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false) || !opts.cdb_path.has_value()) {
        std::ostringstream oss;
        deco::cli::write_usage_for<BenchmarkOptions>(oss, "scan_benchmark [OPTIONS] <cdb>");
        std::print("{}", oss.str());
        return opts.help.value_or(false) ? 0 : 1;
    }

    // Configure logging.
    auto level = spdlog::level::from_str(*opts.log_level);
    clice::logging::options.level = level;
    clice::logging::stderr_logger("scan_benchmark", clice::logging::options);

    // Initialize resource directory (needed for -resource-dir in toolchain queries).
    std::string self_path = llvm::sys::fs::getMainExecutable(argv[0], (void*)main);
    if(!clice::fs::init_resource_dir(self_path)) {
        std::println(stderr, "Warning: failed to find resource dir from {}", self_path);
    }

    auto& cdb_path = *opts.cdb_path;
    auto hw_threads = std::thread::hardware_concurrency();
    auto runs = *opts.runs;

    // Set UV_THREADPOOL_SIZE if not already set.
    // Use at least libuv's default (4) so low-core CI runners don't regress.
    if(!std::getenv("UV_THREADPOOL_SIZE")) {
        auto pool_size = std::max(hw_threads, 4u);
        static std::string env = "UV_THREADPOOL_SIZE=" + std::to_string(pool_size);
        putenv(env.data());
    }

    std::println("Hardware threads: {}", hw_threads);
    std::println("UV_THREADPOOL_SIZE: {}", std::getenv("UV_THREADPOOL_SIZE"));
    std::println("Log level: {}", *opts.log_level);
    std::println("CDB: {}", cdb_path);
    std::println("");

    // Load compilation database.
    auto t0 = std::chrono::steady_clock::now();

    CompilationDatabase cdb;
    auto updates = cdb.load_compile_database(cdb_path);

    auto t1 = std::chrono::steady_clock::now();
    auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::size_t active = 0;
    for(auto& u: updates) {
        if(u.kind != UpdateKind::Deleted) {
            active++;
        }
    }

    std::println("CDB loaded: {} entries ({} active) in {}ms", updates.size(), active, load_ms);

    // ── Context dedup diagnostic ──────────────────────────────────────
    {
        std::size_t total_files = 0;
        std::set<const void*> unique_contexts;
        for(auto& u: updates) {
            if(u.kind != UpdateKind::Deleted) {
                total_files++;
                unique_contexts.insert(u.context);
            }
        }
        std::println("Context dedup: {} files -> {} unique contexts ({:.1f}x reduction)",
                     total_files,
                     unique_contexts.size(),
                     static_cast<double>(total_files) / unique_contexts.size());
    }

    // ── Cold start dependency scan benchmark ──────────────────────────────
    std::println("\nRunning {} cold start scan(s)...\n", runs);

    PathPool path_pool;
    DependencyGraph graph;
    std::vector<std::int64_t> elapsed_times;
    std::vector<std::int64_t> config_times;
    std::vector<std::int64_t> phase1_times;
    std::vector<std::int64_t> phase2_times;
    elapsed_times.reserve(runs);
    config_times.reserve(runs);
    phase1_times.reserve(runs);
    phase2_times.reserve(runs);

    for(int i = 0; i < runs; i++) {
        // True cold start: rebuild CDB (clears toolchain & config caches),
        // reset PathPool and DependencyGraph.
        cdb = CompilationDatabase{};
        updates = cdb.load_compile_database(cdb_path);
        path_pool = PathPool{};
        graph = DependencyGraph{};

        auto report = scan_dependency_graph(cdb, updates, path_pool, graph);

        elapsed_times.push_back(report.elapsed_ms);
        config_times.push_back(report.config_ms);
        phase1_times.push_back(report.phase1_ms);
        phase2_times.push_back(report.phase2_ms);

        std::println("[run {:2}] {}ms | config={}ms phase1={}ms phase2={}ms | files={}",
                     i + 1,
                     report.elapsed_ms,
                     report.config_ms,
                     report.phase1_ms,
                     report.phase2_ms,
                     report.total_files);

        // Print detailed report for the first run only.
        if(i == 0) {
            std::println("");
            print_report(report);
        }
    }

    // Summary statistics.
    if(runs > 1) {
        auto stats = [](std::vector<std::int64_t>& v) {
            std::ranges::sort(v);
            auto sum = std::accumulate(v.begin(), v.end(), std::int64_t{0});
            return std::tuple{v.front(), sum / static_cast<std::int64_t>(v.size()), v.back()};
        };
        auto [e_min, e_avg, e_max] = stats(elapsed_times);
        auto [c_min, c_avg, c_max] = stats(config_times);
        auto [p1_min, p1_avg, p1_max] = stats(phase1_times);
        auto [p2_min, p2_avg, p2_max] = stats(phase2_times);

        std::println("\n  Summary ({} runs)             min    avg    max", runs);
        std::println("    Total:              {:>7} {:>6} {:>6}", e_min, e_avg, e_max);
        std::println("    Config extraction:  {:>7} {:>6} {:>6}", c_min, c_avg, c_max);
        std::println("    Phase 1 (read+scan):{:>7} {:>6} {:>6}", p1_min, p1_avg, p1_max);
        std::println("    Phase 2 (resolve):  {:>7} {:>6} {:>6}", p2_min, p2_avg, p2_max);
    }

    // Export dependency graph as JSON if requested.
    if(opts.export_path.has_value()) {
        export_graph_json(path_pool, graph, *opts.export_path);
    }

    return 0;
}
