/// Benchmark for scan_dependency_graph on a real compilation database.
///
/// Usage:
///   scan_benchmark <path-to-compile_commands.json> [output.json]
///
/// Example:
///   ./build/RelWithDebInfo/bin/scan_benchmark \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json \
///       graph.json

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <print>
#include <thread>

#include "compile/command.h"
#include "eventide/serde/json/serializer.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

namespace et = eventide;

using namespace clice;

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
                       const char* output_path) {
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

    std::ofstream out(output_path);
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

int main(int argc, char* argv[]) {
    if(argc < 2) {
        std::println(stderr, "Usage: {} <compile_commands.json>", argv[0]);
        return 1;
    }

    auto cdb_path = argv[1];
    auto hw_threads = std::thread::hardware_concurrency();

    // Set UV_THREADPOOL_SIZE to hardware concurrency if not already set.
    if(!std::getenv("UV_THREADPOOL_SIZE")) {
        static std::string env = "UV_THREADPOOL_SIZE=" + std::to_string(hw_threads);
        putenv(env.data());
    }

    std::println("Hardware threads: {}", hw_threads);
    std::println("UV_THREADPOOL_SIZE: {}", std::getenv("UV_THREADPOOL_SIZE"));
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

    // Run dependency scan multiple times for warm-cache measurement.
    constexpr int runs = 3;
    std::println("Running {} iterations...\n", runs);

    PathPool path_pool;
    DependencyGraph graph;

    for(int i = 0; i < runs; i++) {
        path_pool = PathPool{};
        graph = DependencyGraph{};

        auto report = scan_dependency_graph(cdb, updates, path_pool, graph);

        std::println("[run {}] {}ms | files={} modules={} edges={}",
                     i + 1,
                     report.elapsed_ms,
                     report.total_files,
                     report.modules,
                     report.total_edges);

        // Print detailed report for the last run.
        if(i == runs - 1) {
            std::println("");
            print_report(report);
        }
    }

    // Export dependency graph as JSON if output path is provided.
    if(argc >= 3) {
        export_graph_json(path_pool, graph, argv[2]);
    }

    return 0;
}
