/// Per-TU profile of the compile pipeline on a real compilation database:
/// how long each stage a language-server pass consists of takes, one result
/// per file.
///
/// Stages, mirroring the server's own build shapes:
///   read               source file I/O
///   preprocess         PreprocessOnlyAction, TokenBuffer off
///   preprocess_tokens  PreprocessOnlyAction, TokenBuffer on (delta = TokenBuffer cost)
///   parse              full parse without PCH + envelope build (the
///                      background-index worker shape)
///   pch_build          preamble PCH build + preamble envelope incl. disk
///                      writes (first didOpen shape)
///   parse_pch          full parse over the PCH + interactive envelope
///                      build (the didChange shape)
///
/// Usage:
///   pipeline_benchmark [OPTIONS] <compile_commands.json>
///
/// Example:
///   ./build/RelWithDebInfo/bin/pipeline_benchmark build/RelWithDebInfo/compile_commands.json
///   ./build/RelWithDebInfo/bin/pipeline_benchmark --filter compiler.cpp --runs 5 \
///       --time-trace /tmp/traces <cdb>

#include <algorithm>
#include <print>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

#include "stats.h"
#include "command/command.h"
#include "compile/compilation.h"
#include "feature/feature.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"

#include "kota/codec/json/json.h"
#include "kota/deco/deco.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/xxhash.h"

using namespace clice;

namespace {

struct BenchmarkOptions {
    DecoKV(names = {"--log-level"}; help = "Log level: trace, debug, info, warn, error, off";
           required = false;)
    <std::string> log_level = "off";

    DecoKV(names = {"--filter"}; help = "Only profile files whose path contains this substring";
           required = false;)
    <std::string> filter;

    DecoKV(names = {"--limit"}; help = "Profile only the N largest files by source size (0 = all)";
           required = false;)
    <int> limit = 0;

    DecoKV(names = {"--runs"}; help = "Repetitions per stage, the minimum is reported";
           required = false;)
    <int> runs = 1;

    DecoKV(names = {"--json"}; help = "Write per-file results as JSON to this path";
           required = false;)
    <std::string> json_path;

    DecoKV(names = {"--time-trace"};
           help = "Write a Chrome trace of each file's parse stage into this directory";
           required = false;)
    <std::string> time_trace_dir;

    DecoFlag(names = {"-h", "--help"}; help = "Show help message"; required = false;)
    help;

    DecoInput(meta_var = "CDB"; help = "Path to compile_commands.json"; required = false;)
    <std::string> cdb_path;
};

/// One file, one result. A stage that did not run stays at -1 (e.g. the
/// PCH stages of a file with an empty preamble, or everything after a
/// failed stage).
struct FileResult {
    std::string file;
    std::string error;

    double read_ms = -1;
    double preprocess_ms = -1;
    double preprocess_tokens_ms = -1;
    double parse_ms = -1;
    double index_ms = -1;
    double pch_build_ms = -1;
    double parse_pch_ms = -1;

    std::uint64_t index_bytes = 0;
    std::uint64_t pch_bytes = 0;
    std::uint64_t symbols = 0;
    std::uint32_t preamble_bound = 0;
};

struct BenchmarkOutput {
    std::string cdb;
    std::vector<FileResult> files;
};

CompilationParams make_params(CompilationKind kind,
                              const std::vector<const char*>& arguments,
                              llvm::StringRef file,
                              llvm::StringRef content) {
    CompilationParams params;
    params.kind = kind;
    params.arguments = arguments;
    params.add_remapped_file(file, content);
    return params;
}

/// Run `stage` `runs` times and keep the fastest wall-clock time. The
/// stage reports failure by returning false; timing of a failed run is
/// discarded.
template <typename Stage>
bool run_stage(int runs, double& out_ms, Stage stage) {
    double best = -1;
    for(int i = 0; i < runs; i += 1) {
        ScopedTimer timer;
        if(!stage()) {
            return false;
        }
        auto ms = timer.ms_f();
        if(best < 0 || ms < best) {
            best = ms;
        }
    }
    out_ms = best;
    return true;
}

FileResult profile_file(llvm::StringRef file,
                        const std::vector<const char*>& arguments,
                        int runs,
                        llvm::StringRef time_trace_dir) {
    FileResult result;
    result.file = file.str();

    std::string content;
    bool ok = run_stage(runs, result.read_ms, [&] {
        auto read = fs::read(file);
        if(!read) {
            result.error = "read failed: " + read.error().message();
            return false;
        }
        content = std::move(*read);
        return true;
    });
    if(!ok) {
        return result;
    }

    // One unmeasured preprocess warms the OS caches for every header the
    // TU touches; without it the first measured variant pays the cold-cache
    // cost and the tokens-vs-no-tokens delta is polluted.
    {
        auto params = make_params(CompilationKind::Preprocess, arguments, file, content);
        preprocess(params);
    }

    for(bool collect_tokens: {false, true}) {
        auto& out_ms = collect_tokens ? result.preprocess_tokens_ms : result.preprocess_ms;
        ok = run_stage(runs, out_ms, [&] {
            auto params = make_params(CompilationKind::Preprocess, arguments, file, content);
            params.collect_tokens = collect_tokens;
            auto unit = preprocess(params);
            // completed() only covers frontend execution; missing headers,
            // bad flags and ordinary source errors surface as diagnostics
            // on a completed unit, and such a run's timing must not enter
            // the samples (same for every acceptance check below).
            if(auto errors = collect_errors(unit); !unit.completed() || !errors.empty()) {
                result.error = "preprocess failed: " + errors;
                return false;
            }
            return true;
        });
        if(!ok) {
            return result;
        }
    }

    // The background-index worker shape: plain parse, then index build and
    // serialization on the same unit. When a trace directory is given the
    // profiler window covers all three; clang only emits events during the
    // parse, and the trace must be written after the unit is destroyed —
    // EndSourceFile runs in the unit's destructor.
    bool tracing = !time_trace_dir.empty();
    if(tracing) {
        llvm::timeTraceProfilerInitialize(/*TimeTraceGranularity=*/500, "pipeline_benchmark");
    }
    ok = run_stage(tracing ? 1 : runs, result.parse_ms, [&] {
        auto params = make_params(CompilationKind::Indexing, arguments, file, content);
        auto unit = compile(params);
        if(auto errors = collect_errors(unit); !unit.completed() || !errors.empty()) {
            result.error = "parse failed: " + errors;
            return false;
        }

        // Like the stage itself, keep the fastest run for the sub-timings.
        auto keep_min = [](double& slot, double ms) {
            if(slot < 0 || ms < slot) {
                slot = ms;
            }
        };

        ScopedTimer index_timer;
        auto serialized = index::build_tu_index(unit);
        keep_min(result.index_ms, index_timer.ms_f());
        result.index_bytes = serialized.size();

        auto view = index::TUIndex::from_bytes(serialized);
        std::uint64_t symbols = 0;
        view.iterate_symbols([&](auto, auto&, auto) {
            symbols += 1;
            return true;
        });
        result.symbols = symbols;
        return true;
    });
    if(tracing) {
        llvm::SmallString<128> trace_path{time_trace_dir};
        // Duplicate basenames are common in large projects; a full-path
        // hash keeps one trace per TU.
        llvm::sys::path::append(trace_path,
                                std::format("{}.{:08x}.json",
                                            llvm::sys::path::filename(file).str(),
                                            static_cast<std::uint32_t>(llvm::xxh3_64bits(file))));
        if(auto error = llvm::timeTraceProfilerWrite(trace_path, file)) {
            LOG_WARN("Failed to write time trace {}: {}", trace_path, error);
        }
        llvm::timeTraceProfilerCleanup();
    }
    if(!ok) {
        // The parse timing subsumes index build; a parse that failed halfway
        // still leaves the earlier stage numbers meaningful, so report them.
        return result;
    }

    // The interactive shapes: build the preamble PCH (first didOpen), then
    // parse the full file over it (every didChange).
    result.preamble_bound = compute_preamble_bound(content);
    if(result.preamble_bound == 0) {
        return result;
    }

    auto pch_path = fs::createTemporaryFile("pipeline-bench", "pch");
    auto state_path = fs::createTemporaryFile("pipeline-bench", "idx");
    if(!pch_path || !state_path) {
        result.error = "failed to create temporary PCH files";
        return result;
    }
    auto remove_pch_files = [&] {
        fs::remove(*pch_path);
        fs::remove(*state_path);
    };

    std::vector<std::uint8_t> open_conditionals;
    ok = run_stage(runs, result.pch_build_ms, [&] {
        CompilationParams params;
        params.kind = CompilationKind::Preamble;
        params.arguments = arguments;
        params.add_remapped_file(file, content, result.preamble_bound);
        params.output_file = *pch_path;

        PCHInfo pch_info;
        auto unit = compile(params, pch_info);
        if(auto errors = collect_errors(unit); !unit.completed() || !errors.empty()) {
            result.error = "pch build failed: " + errors;
            return false;
        }

        // The production PCH pass (stateless worker) also builds the
        // preamble's envelope, document links and inactive regions and
        // writes the blob next to the PCH before reporting success; the
        // stage must carry that cost to match a didOpen.
        auto links = feature::document_links(unit);
        auto inactive = feature::inactive_regions(unit, {}, 0, result.preamble_bound);
        open_conditionals = std::move(inactive.open_stack);
        auto blob = index::build_preamble_index(unit, links, inactive.regions, open_conditionals);

        // The PCH is flushed to disk by the unit's destructor; the blob
        // write follows it, like the worker's on-disk ordering contract.
        unit = CompilationUnit(nullptr);
        if(auto write = fs::write(*state_path, blob); !write) {
            result.error = "preamble state write failed: " + write.error().message();
            return false;
        }
        return true;
    });
    if(!ok) {
        remove_pch_files();
        return result;
    }

    if(auto error = llvm::sys::fs::file_size(*pch_path, result.pch_bytes)) {
        result.pch_bytes = 0;
    }

    run_stage(runs, result.parse_pch_ms, [&] {
        auto params = make_params(CompilationKind::Content, arguments, file, content);
        params.pch = {*pch_path, result.preamble_bound};
        auto unit = compile(params);
        if(auto errors = collect_errors(unit); !unit.completed() || !errors.empty()) {
            result.error = "parse with pch failed: " + errors;
            return false;
        }

        // The didChange pass (stateful worker) also computes inactive
        // regions and builds the interested-only envelope before replying;
        // include them so parse and parse_pch bound the same work.
        feature::inactive_regions(unit, open_conditionals, result.preamble_bound);
        index::build_tu_index(unit, /*interested_only=*/true);
        return true;
    });

    remove_pch_files();
    return result;
}

void print_file(const FileResult& result) {
    auto cell = [](double ms) {
        return ms < 0 ? std::string("      -") : std::format("{:>7.1f}", ms);
    };
    std::println("  {} {} {} {} {} {} {}  {}",
                 cell(result.preprocess_ms),
                 cell(result.preprocess_tokens_ms),
                 cell(result.parse_ms),
                 cell(result.index_ms),
                 cell(result.pch_build_ms),
                 cell(result.parse_pch_ms),
                 std::format("{:>8}", result.symbols),
                 result.file);
}

struct StageStats {
    llvm::StringRef name;
    std::vector<double> values;

    void add(double ms) {
        if(ms >= 0) {
            values.push_back(ms);
        }
    }
};

void print_summary(std::vector<FileResult>& results) {
    StageStats stats[] = {
        {"read"},
        {"preprocess"},
        {"preprocess_tokens"},
        {"parse"},
        {"index"},
        {"pch_build"},
        {"parse_pch"},
    };
    for(auto& result: results) {
        stats[0].add(result.read_ms);
        stats[1].add(result.preprocess_ms);
        stats[2].add(result.preprocess_tokens_ms);
        stats[3].add(result.parse_ms);
        stats[4].add(result.index_ms);
        stats[5].add(result.pch_build_ms);
        stats[6].add(result.parse_pch_ms);
    }

    std::println("");
    std::println("  Stage summary (ms){:>14} {:>8} {:>8} {:>8} {:>8}",
                 "files",
                 "p50",
                 "p90",
                 "p99",
                 "max");
    for(auto& stage: stats) {
        if(stage.values.empty()) {
            continue;
        }
        std::ranges::sort(stage.values);
        std::println("    {:<17}{:>13} {:>8.1f} {:>8.1f} {:>8.1f} {:>8.1f}",
                     stage.name.str(),
                     stage.values.size(),
                     bench::percentile(stage.values, 0.5),
                     bench::percentile(stage.values, 0.9),
                     bench::percentile(stage.values, 0.99),
                     stage.values.back());
    }

    auto slowest = results | std::views::filter([](auto& r) { return r.parse_ms >= 0; }) |
                   std::ranges::to<std::vector>();
    std::ranges::sort(slowest, std::greater{}, &FileResult::parse_ms);
    std::println("");
    std::println("  Slowest TUs by parse time");
    for(auto& result: slowest | std::views::take(10)) {
        std::println("    {:>8.1f}ms  {}", result.parse_ms, result.file);
    }

    auto failed = std::ranges::count_if(results, [](auto& r) { return !r.error.empty(); });
    if(failed > 0) {
        std::println("");
        std::println("  {} file(s) had a failing stage (see JSON `error` fields)", failed);
    }
}

}  // namespace

int main(int argc, const char** argv) {
    auto args = kota::deco::util::argvify(argc, argv);
    auto result = kota::deco::cli::parse<BenchmarkOptions>(args);

    if(!result.has_value()) {
        std::println(stderr, "Error: {}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false) || !opts.cdb_path.has_value()) {
        std::ostringstream oss;
        kota::deco::cli::write_usage_for<BenchmarkOptions>(oss,
                                                           "pipeline_benchmark [OPTIONS] <cdb>");
        std::print("{}", oss.str());
        return opts.help.value_or(false) ? 0 : 1;
    }

    auto level = spdlog::level::from_str(*opts.log_level);
    clice::logging::options.level = level;
    clice::logging::stderr_logger("pipeline_benchmark", clice::logging::options);

    auto runs = *opts.runs;
    if(runs <= 0) {
        std::println(stderr, "Error: --runs must be positive (got {})", runs);
        return 1;
    }

    if(opts.time_trace_dir.has_value()) {
        if(auto error = llvm::sys::fs::create_directories(*opts.time_trace_dir)) {
            std::println(stderr, "Error: cannot create {}: {}", *opts.time_trace_dir, error);
            return 1;
        }
    }

    CompilationDatabase cdb;
    auto count = cdb.load(*opts.cdb_path);
    if(!count) {
        std::println(stderr, "Error: failed to load {}", *opts.cdb_path);
        return 1;
    }
    std::println("CDB loaded: {} entries", *count);

    // One profile per file: entries are sorted by path id, and a file with
    // several commands (multi-config CDB) is profiled under the first one,
    // like the server picks.
    std::vector<llvm::StringRef> files;
    for(auto& entry: cdb.entries()) {
        auto path = cdb.paths().resolve(entry.file);
        if(opts.filter.has_value() && !path.contains(*opts.filter)) {
            continue;
        }
        if(!files.empty() && files.back() == path) {
            continue;
        }
        files.push_back(path);
    }

    // --limit keeps the N largest TUs by main-file size — a coarse but
    // stable proxy for TU weight — rather than an arbitrary CDB prefix.
    if(*opts.limit > 0 && files.size() > static_cast<std::size_t>(*opts.limit)) {
        std::vector<std::pair<std::uint64_t, llvm::StringRef>> sized;
        sized.reserve(files.size());
        for(auto path: files) {
            std::uint64_t size = 0;
            if(auto error = llvm::sys::fs::file_size(path, size)) {
                size = 0;
            }
            sized.emplace_back(size, path);
        }
        std::ranges::stable_sort(sized, std::greater{}, [](auto& pair) { return pair.first; });
        files.clear();
        for(auto& [size, path]: sized | std::views::take(*opts.limit)) {
            files.push_back(path);
        }
    }
    std::println("Profiling {} file(s), {} run(s) per stage\n", files.size(), runs);

    std::println("  {:>7} {:>7} {:>7} {:>7} {:>7} {:>7} {:>8}  file",
                 "pp",
                 "pp+tok",
                 "parse",
                 "index",
                 "pch",
                 "reparse",
                 "symbols");

    BenchmarkOutput output;
    output.cdb = *opts.cdb_path;
    for(auto file: files) {
        auto candidates = cdb.candidate_entries(file);
        if(candidates.empty()) {
            continue;
        }
        auto& entry = candidates.front();
        CommandRef ref{entry.file,
                       entry.config,
                       cdb.input_kind(entry.config, file),
                       CommandSource::CDBExact};
        auto arguments = cdb.render(ref);

        auto file_result = profile_file(file, arguments, runs, opts.time_trace_dir.value_or(""));
        print_file(file_result);
        output.files.push_back(std::move(file_result));
    }

    print_summary(output.files);

    if(opts.json_path.has_value()) {
        auto json = kota::codec::json::to_string(output);
        if(!json) {
            std::println(stderr, "Failed to serialize results");
            return 1;
        }
        if(auto write = fs::write(*opts.json_path, *json); !write) {
            std::println(stderr,
                         "Failed to write {}: {}",
                         *opts.json_path,
                         write.error().message());
            return 1;
        }
        std::println("\nResults written to {}", *opts.json_path);
    }

    return 0;
}
