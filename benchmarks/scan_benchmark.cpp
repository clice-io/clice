/// Benchmark for scan_dependency_graph on a real compilation database.
///
/// Usage:
///   scan_benchmark [OPTIONS] <compile_commands.json>
///
/// Example:
///   ./build/RelWithDebInfo/bin/bin/scan_benchmark \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json
///
///   ./build/RelWithDebInfo/bin/bin/scan_benchmark --log-level info --export graph.json \
///       /home/ykiko/C++/clice/.llvm/build-debug/compile_commands.json

module;

#include "modules/prelude.h"

module clice;

import :command.command;
import :config.config;
import :project.build;
import :support.logging;
import :syntax.dependency_graph;
import :vfs.file_table;

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

void export_graph_json(const FileTable& file_table,
                       const DependencyGraph& graph,
                       llvm::StringRef output_path) {
    // Build reverse module map: path_id -> module_name.
    llvm::DenseMap<Fid, llvm::StringRef> path_to_module;
    for(auto& [name, path_ids]: graph.modules()) {
        for(auto path_id: path_ids) {
            path_to_module[path_id] = name;
        }
    }

    GraphExport export_data;
    for(std::uint32_t i = 0; i < file_table.spellings.size(); i += 1) {
        auto id = Fid{i};
        auto inc_ids = graph.get_all_includes(id);
        if(inc_ids.empty()) {
            continue;
        }

        FileNode node;
        node.path = file_table.resolve(id).str();

        auto mod_it = path_to_module.find(id);
        if(mod_it != path_to_module.end()) {
            node.module_name = mod_it->second.str();
        }

        for(auto inc: inc_ids) {
            node.includes.push_back(file_table.resolve(inc).str());
        }

        export_data.files.push_back(std::move(node));
    }

    auto json = kota::codec::json::to_string(export_data);
    if(!json) {
        std::println(stderr, "Failed to serialize dependency graph");
        return;
    }

    std::ofstream out(output_path.str());
    if(!out) {
        std::println(stderr, "Failed to open output file: {}", output_path);
        return;
    }
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
    std::println("    Header files (discovered): {}", report.total_files - report.source_files);
    std::println("    Total:                     {}", report.total_files);
    std::println("    Scan result cache hits:    {}", report.scan_cache_hits);

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

    std::println("");
    std::println("===============================================================");
}

extern "C++" int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto result = kota::deco::cli::parse<BenchmarkOptions>(args);

    if(!result.has_value()) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false) || !opts.cdb_path.has_value()) {
        std::ostringstream oss;
        kota::deco::cli::write_usage_for<BenchmarkOptions>(oss, "scan_benchmark [OPTIONS] <cdb>");
        std::print("{}", oss.str());
        return opts.help.value_or(false) ? 0 : 1;
    }

    // Configure logging.
    auto level = spdlog::level::from_str(*opts.log_level);
    clice::logging::options.level = level;
    clice::logging::stderr_logger("scan_benchmark", clice::logging::options);

    // resource_dir() is self-initializing (lazy static) — no setup needed.

    auto& cdb_path = *opts.cdb_path;
    auto hw_threads = std::thread::hardware_concurrency();
    auto runs = *opts.runs;
    if(runs <= 0) {
        std::println(stderr, "Error: --runs must be positive (got {})", runs);
        return 1;
    }

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

    std::optional<FileTable> table_storage;
    std::optional<CompilationDatabase> cdb_storage;
    table_storage.emplace();
    cdb_storage.emplace(*table_storage);
    auto& cdb = *cdb_storage;
    auto loaded = cdb.load(cdb_path);
    if(!loaded) {
        std::println(stderr, "Error: failed to load {}", cdb_path);
        return 1;
    }
    auto count = *loaded;

    auto t1 = std::chrono::steady_clock::now();
    auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    std::println("CDB loaded: {} entries in {}ms", count, load_ms);

    {
        std::set<ConfigID> unique_configs;
        for(auto& entry: cdb.entries()) {
            unique_configs.insert(entry.config);
        }
        double dedup_ratio =
            unique_configs.empty() ? 0.0 : static_cast<double>(count) / unique_configs.size();
        std::println("Config dedup: {} files -> {} unique configs ({:.1f}x)",
                     count,
                     unique_configs.size(),
                     dedup_ratio);
    }

    std::println("\nRunning {} cold start scan(s)...\n", runs);

    Config config;
    DependencyGraph graph;
    std::vector<std::int64_t> elapsed_times;
    elapsed_times.reserve(runs);

    for(int i = 0; i < runs; i += 1) {
        // True cold start: rebuild CDB (clears toolchain & config caches)
        // and the DependencyGraph.
        cdb_storage.reset();
        table_storage.emplace();
        cdb_storage.emplace(*table_storage);
        cdb_storage->load(cdb_path);
        graph = DependencyGraph{};
        Build build{config, *cdb_storage, *table_storage};
        build.reset_active("");

        auto report = scan_dependency_graph(*cdb_storage, graph, build.units(build.members()));

        elapsed_times.push_back(report.elapsed_ms);

        std::println("[run {:2}] {}ms | files={}", i + 1, report.elapsed_ms, report.total_files);

        // Print detailed report for the first run only.
        if(i == 0) {
            std::println("");
            print_report(report);
        }
    }

    // Summary statistics.
    if(runs > 1) {
        std::ranges::sort(elapsed_times);
        auto sum = std::accumulate(elapsed_times.begin(), elapsed_times.end(), std::int64_t{0});
        std::println("\n  Summary ({} runs)             min    avg    max", runs);
        std::println("    Total:              {:>7} {:>6} {:>6}",
                     elapsed_times.front(),
                     sum / runs,
                     elapsed_times.back());
    }

    // Export dependency graph as JSON if requested.
    if(opts.export_path.has_value()) {
        export_graph_json(cdb_storage->files(), graph, *opts.export_path);
    }

    return 0;
}
