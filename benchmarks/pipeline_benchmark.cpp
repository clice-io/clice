/// Per-TU profile of the compile pipeline on a real compilation database:
/// how long each stage a language-server pass consists of takes, one result
/// per file.
///
/// Stages, mirroring the server's own build shapes:
///   read               source file I/O
///   preprocess         PreprocessOnlyAction, TokenBuffer off
///   preprocess_tokens  PreprocessOnlyAction, TokenBuffer on (delta = TokenBuffer cost)
///   parse              full parse without PCH + TUIndex build + serialize
///                      (the background-index worker shape)
///   pch_build          preamble PCH build incl. disk flush (first didOpen shape)
///   parse_pch          full parse over the PCH (the didChange shape)
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

#include "command/command.h"
#include "command/toolchain.h"
#include "compile/compilation.h"
#include "index/tu_index.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "syntax/scan.h"

#include "kota/codec/json/json.h"
#include "kota/deco/deco.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TimeProfiler.h"

using namespace clice;

namespace {

struct BenchmarkOptions {
    DecoKV(names = {"--log-level"}; help = "Log level: trace, debug, info, warn, error, off";
           required = false;)
    <std::string> log_level = "off";

    DecoKV(names = {"--filter"}; help = "Only profile files whose path contains this substring";
           required = false;)
    <std::string> filter;

    DecoKV(names = {"--limit"}; help = "Profile at most N files (0 = all)"; required = false;)
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
    double index_serialize_ms = -1;
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

std::string collect_errors(CompilationUnit& unit) {
    std::string errors;
    for(auto& diag: unit.diagnostics()) {
        if(diag.id.level >= DiagnosticLevel::Error) {
            if(!errors.empty())
                errors += "; ";
            errors += diag.message;
        }
    }
    return errors;
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

    for(bool collect_tokens: {false, true}) {
        auto& out_ms = collect_tokens ? result.preprocess_tokens_ms : result.preprocess_ms;
        ok = run_stage(runs, out_ms, [&] {
            auto params = make_params(CompilationKind::Preprocess, arguments, file, content);
            params.collect_tokens = collect_tokens;
            auto unit = preprocess(params);
            if(!unit.completed()) {
                result.error = "preprocess failed: " + collect_errors(unit);
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
        if(!unit.completed()) {
            result.error = "parse failed: " + collect_errors(unit);
            return false;
        }

        // Like the stage itself, keep the fastest run for the sub-timings.
        auto keep_min = [](double& slot, double ms) {
            if(slot < 0 || ms < slot) {
                slot = ms;
            }
        };

        ScopedTimer index_timer;
        auto tu_index = index::TUIndex::build(unit);
        keep_min(result.index_ms, index_timer.ms_f());
        result.symbols = tu_index.symbols.size();

        ScopedTimer serialize_timer;
        std::string serialized;
        llvm::raw_string_ostream os(serialized);
        tu_index.serialize(os);
        keep_min(result.index_serialize_ms, serialize_timer.ms_f());
        result.index_bytes = serialized.size();
        return true;
    });
    if(tracing) {
        llvm::SmallString<128> trace_path{time_trace_dir};
        llvm::sys::path::append(trace_path, llvm::sys::path::filename(file));
        trace_path += ".json";
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
    if(!pch_path) {
        result.error = "failed to create temporary PCH file";
        return result;
    }

    ok = run_stage(runs, result.pch_build_ms, [&] {
        CompilationParams params;
        params.kind = CompilationKind::Preamble;
        params.arguments = arguments;
        params.add_remapped_file(file, content, result.preamble_bound);
        params.output_file = *pch_path;

        PCHInfo pch_info;
        auto unit = compile(params, pch_info);
        if(!unit.completed()) {
            result.error = "pch build failed: " + collect_errors(unit);
            return false;
        }
        // The PCH is flushed to disk by the unit's destructor; the stage
        // timing must include it, so destroy explicitly.
        unit = CompilationUnit(nullptr);
        return true;
    });
    if(!ok) {
        fs::remove(*pch_path);
        return result;
    }

    if(auto error = llvm::sys::fs::file_size(*pch_path, result.pch_bytes)) {
        result.pch_bytes = 0;
    }

    run_stage(runs, result.parse_pch_ms, [&] {
        auto params = make_params(CompilationKind::Content, arguments, file, content);
        params.pch = {*pch_path, result.preamble_bound};
        auto unit = compile(params);
        if(!unit.completed()) {
            result.error = "parse with pch failed: " + collect_errors(unit);
            return false;
        }
        return true;
    });

    fs::remove(*pch_path);
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

    double percentile(double p) {
        auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
        return values[index];
    }
};

void print_summary(std::vector<FileResult>& results) {
    StageStats stats[] = {
        {"read"},
        {"preprocess"},
        {"preprocess_tokens"},
        {"parse"},
        {"index"},
        {"index_serialize"},
        {"pch_build"},
        {"parse_pch"},
    };
    for(auto& result: results) {
        stats[0].add(result.read_ms);
        stats[1].add(result.preprocess_ms);
        stats[2].add(result.preprocess_tokens_ms);
        stats[3].add(result.parse_ms);
        stats[4].add(result.index_ms);
        stats[5].add(result.index_serialize_ms);
        stats[6].add(result.pch_build_ms);
        stats[7].add(result.parse_pch_ms);
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
                     stage.percentile(0.5),
                     stage.percentile(0.9),
                     stage.percentile(0.99),
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
    Toolchain toolchain;
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
    for(auto& entry: cdb.get_entries()) {
        auto path = cdb.resolve_path(entry.file);
        if(opts.filter.has_value() && !path.contains(*opts.filter)) {
            continue;
        }
        if(!files.empty() && files.back() == path) {
            continue;
        }
        files.push_back(path);
        if(*opts.limit > 0 && files.size() == static_cast<std::size_t>(*opts.limit)) {
            break;
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
        auto commands = cdb.lookup(file);
        if(commands.empty()) {
            continue;
        }
        toolchain.resolve_or_warn(commands[0]);
        auto arguments = commands[0].to_argv();

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
