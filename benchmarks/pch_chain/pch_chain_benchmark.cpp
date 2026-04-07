/// Benchmark: Monolithic PCH vs Chained PCH using clice/clang compilation APIs.
///
/// Tests:
///   1. Correctness of chained PCH with all C++ standard library headers
///   2. Build time: monolithic (single PCH) vs chained (one-per-include)
///   3. Incremental rebuild: appending one header to existing chain
///   4. Compile-with-PCH latency for both strategies
///
/// Usage:
///   pch_chain_benchmark [--runs N] [--chain-length N]
///
/// Requires: clice built with CLICE_ENABLE_BENCHMARK=ON

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <numeric>
#include <print>
#include <string>
#include <vector>

#include "command/command.h"
#include "compile/compilation.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace clice;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// All C++20 standard library headers in a reasonable include order.
/// Ordered to minimize dependencies (basic types first, I/O last).
const static std::vector<std::string> ALL_HEADERS = {
    "cstddef",
    "cstdint",
    "climits",
    "cfloat",
    "type_traits",
    "concepts",
    "compare",
    "initializer_list",
    "utility",
    "tuple",
    "optional",
    "variant",
    "any",
    "expected",
    "bitset",
    "bit",
    "string_view",
    "string",
    "charconv",
    "format",
    "array",
    "vector",
    "deque",
    "list",
    "forward_list",
    "set",
    "map",
    "unordered_set",
    "unordered_map",
    "stack",
    "queue",
    "span",
    "iterator",
    "ranges",
    "algorithm",
    "numeric",
    "memory",
    "memory_resource",
    "scoped_allocator",
    "functional",
    "ratio",
    "chrono",
    "exception",
    "stdexcept",
    "system_error",
    "typeinfo",
    "typeindex",
    "source_location",
    "new",
    "limits",
    "numbers",
    "valarray",
    "complex",
    "random",
    "iosfwd",
    "ios",
    "streambuf",
    "istream",
    "ostream",
    "iostream",
    "sstream",
    "fstream",
    "cmath",
    "cstdio",
    "cstdlib",
    "cstring",
    "ctime",
    "cassert",
    "cerrno",
    "atomic",
    "mutex",
    "condition_variable",
    "thread",
    "future",
    "semaphore",
    "latch",
    "barrier",
    "stop_token",
    "shared_mutex",
    "regex",
    "filesystem",
    "locale",
    "codecvt",
};

/// Generate preamble text for N headers.
static std::string make_preamble(const std::vector<std::string>& headers, std::size_t count) {
    std::string text;
    for(std::size_t i = 0; i < count && i < headers.size(); ++i) {
        text += "#include <" + headers[i] + ">\n";
    }
    return text;
}

/// Create a temporary file path with given extension.
static std::string temp_path(llvm::StringRef prefix, llvm::StringRef ext) {
    auto result = fs::createTemporaryFile(prefix, ext);
    if(!result) {
        std::println(stderr, "Failed to create temp file");
        std::exit(1);
    }
    return *result;
}

/// Build compiler arguments for PCH generation.
static std::vector<const char*> make_pch_args(const std::string& file,
                                              const std::string& resource_dir) {
    static std::vector<std::string> storage;
    storage = {"clang++", "-std=c++20", "-resource-dir", resource_dir, "-x", "c++-header", file};
    std::vector<const char*> args;
    for(auto& s: storage)
        args.push_back(s.c_str());
    return args;
}

// ---------------------------------------------------------------------------
// PCH building helpers
// ---------------------------------------------------------------------------

struct PCHBuildResult {
    bool success = false;
    std::string path;
    double ms = 0;
    std::size_t size_bytes = 0;
    std::string error;
};

/// Build a single PCH (monolithic or chain link).
static PCHBuildResult build_one_pch(const std::string& header_text,
                                    const std::string& file_path,
                                    const std::string& output_path,
                                    const std::string& resource_dir,
                                    const std::string& prev_pch = "",
                                    std::uint32_t prev_bound = 0) {
    PCHBuildResult result;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    cp.output_file = output_path;
    cp.directory = "/tmp";

    auto args = make_pch_args(file_path, resource_dir);
    cp.arguments = args;

    cp.add_remapped_file(file_path, header_text);

    if(!prev_pch.empty()) {
        cp.pch = {prev_pch, prev_bound};
    }

    auto start = Clock::now();

    PCHInfo pch_info;
    auto unit = compile(cp, pch_info);
    bool ok = unit.completed();

    auto end = Clock::now();
    result.ms = std::chrono::duration<double, std::milli>(end - start).count();

    if(ok) {
        // Flush to disk by destroying unit.
        unit = CompilationUnit(nullptr);
        if(llvm::sys::fs::exists(output_path)) {
            result.success = true;
            result.path = output_path;
            std::uint64_t sz;
            llvm::sys::fs::file_size(output_path, sz);
            result.size_bytes = sz;
        } else {
            result.success = false;
            result.error = "PCH file not written to disk";
        }
    } else {
        // Collect diagnostic errors before destroying unit.
        std::string errors;
        for(auto& diag: unit.diagnostics()) {
            if(static_cast<int>(diag.id.level) >= 3) {
                if(!errors.empty())
                    errors += "; ";
                errors += diag.message;
            }
        }
        unit = CompilationUnit(nullptr);
        result.success = false;
        result.error = errors.empty() ? "PCH compilation failed (no diagnostics)" : errors;
    }

    return result;
}

/// Verify a PCH works by syntax-checking a test file against it.
constexpr const static char* VERIFY_FILE = "/tmp/pch-verify.cpp";
constexpr const static char* VERIFY_CODE =
    "static_assert(sizeof(int) > 0);\nint main() { return 0; }\n";

static bool verify_pch(const std::string& pch_path,
                       std::uint32_t preamble_bound,
                       const std::string& resource_dir) {
    CompilationParams cp;
    cp.kind = CompilationKind::Content;
    cp.directory = "/tmp";

    std::vector<std::string> arg_storage =
        {"clang++", "-std=c++20", "-resource-dir", resource_dir, "-fsyntax-only", VERIFY_FILE};
    std::vector<const char*> args;
    for(auto& s: arg_storage)
        args.push_back(s.c_str());
    cp.arguments = args;

    cp.add_remapped_file(VERIFY_FILE, VERIFY_CODE);
    cp.pch = {pch_path, preamble_bound};

    auto unit = compile(cp);
    bool ok = unit.completed();
    unit = CompilationUnit(nullptr);
    return ok;
}

// ---------------------------------------------------------------------------
// Benchmark routines
// ---------------------------------------------------------------------------

static void bench_monolithic(const std::vector<std::string>& headers,
                             std::size_t count,
                             int runs,
                             const std::string& resource_dir) {
    std::println("=== MONOLITHIC PCH ({} headers, {} runs) ===", count, runs);

    std::string preamble = make_preamble(headers, count);
    std::string file_path = temp_path("mono-preamble", "h");
    std::string pch_path = temp_path("mono", "pch");

    std::vector<double> times;
    times.reserve(runs);

    for(int r = 0; r < runs; ++r) {
        fs::remove(pch_path);
        auto result = build_one_pch(preamble, file_path, pch_path, resource_dir);
        if(!result.success) {
            std::println(stderr, "  Run {}: FAILED - {}", r + 1, result.error);
            break;
        }
        times.push_back(result.ms);
        if(r == 0) {
            std::println("  Size: {} KB", result.size_bytes / 1024);
            // Verify correctness.
            auto bound = static_cast<std::uint32_t>(preamble.size());
            bool ok = verify_pch(pch_path, bound, resource_dir);
            std::println("  Correctness: {}", ok ? "PASS" : "FAIL");
        }
    }

    if(!times.empty()) {
        std::sort(times.begin(), times.end());
        double median = times[times.size() / 2];
        double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        double min = times.front();
        double max = times.back();
        std::println("  Min: {:.1f}ms  Median: {:.1f}ms  Mean: {:.1f}ms  Max: {:.1f}ms",
                     min,
                     median,
                     mean,
                     max);
    }

    fs::remove(file_path);
    fs::remove(pch_path);
}

static void bench_chained(const std::vector<std::string>& headers,
                          std::size_t count,
                          int runs,
                          const std::string& resource_dir) {
    std::println("\n=== CHAINED PCH ({} headers, {} runs) ===", count, runs);

    // For chained: build chain once, report per-link times on first run,
    // then rebuild entire chain for timing on subsequent runs.

    struct LinkInfo {
        std::string header;
        std::string file_path;
        std::string pch_path;
        double build_ms = 0;
        std::size_t size_bytes = 0;
        bool success = false;
    };

    auto build_chain = [&](bool verbose) -> std::vector<LinkInfo> {
        std::vector<LinkInfo> links;
        links.reserve(count);

        std::string prev_pch;
        std::uint32_t prev_bound = 0;

        for(std::size_t i = 0; i < count && i < headers.size(); ++i) {
            LinkInfo link;
            link.header = headers[i];

            std::string text = "#include <" + headers[i] + ">\n";
            link.file_path = temp_path("chain-link", "h");
            link.pch_path = temp_path("chain", "pch");

            auto result = build_one_pch(text,
                                        link.file_path,
                                        link.pch_path,
                                        resource_dir,
                                        prev_pch,
                                        prev_bound);
            link.build_ms = result.ms;
            link.size_bytes = result.size_bytes;
            link.success = result.success;

            if(verbose) {
                if(result.success) {
                    std::println("  [{:3}/{}] <{:25}> {:7.1f}ms  {:5} KB",
                                 i + 1,
                                 count,
                                 headers[i],
                                 result.ms,
                                 result.size_bytes / 1024);
                } else {
                    std::println("  [{:3}/{}] <{:25}> FAILED: {}",
                                 i + 1,
                                 count,
                                 headers[i],
                                 result.error);
                }
            }

            if(!result.success) {
                // Skip this link, continue with previous PCH.
                fs::remove(link.file_path);
                links.push_back(std::move(link));
                continue;
            }

            prev_pch = link.pch_path;
            // For chained PCH, bound = 0: the previous PCH does NOT cover
            // any bytes of the CURRENT file.  PrecompiledPreambleBytes tells
            // clang to skip the first N bytes of the current source, which
            // is only correct when the PCH was built FROM the current file
            // (monolithic preamble).  In chained mode each link is a
            // separate file, so nothing should be skipped.
            prev_bound = 0;
            links.push_back(std::move(link));
        }

        return links;
    };

    // First run: verbose, report per-link times.
    auto links = build_chain(true);

    std::size_t passed = 0, failed = 0;
    double total_ms = 0;
    for(auto& l: links) {
        if(l.success) {
            ++passed;
            total_ms += l.build_ms;
        } else {
            ++failed;
        }
    }

    std::println("\n  Chain result: {} passed, {} failed, total {:.1f}ms",
                 passed,
                 failed,
                 total_ms);

    // Verify final PCH.
    if(!links.empty() && links.back().success) {
        // Chained PCH: bound=0 because the PCH doesn't cover the test file.
        bool ok = verify_pch(links.back().pch_path, 0, resource_dir);
        std::println("  Final PCH correctness: {}", ok ? "PASS" : "FAIL");
    }

    // Additional runs for timing statistics.
    if(runs > 1) {
        std::vector<double> totals;
        totals.push_back(total_ms);

        for(int r = 1; r < runs; ++r) {
            // Cleanup previous chain.
            for(auto& l: links) {
                fs::remove(l.file_path);
                fs::remove(l.pch_path);
            }
            auto chain = build_chain(false);
            double t = 0;
            for(auto& l: chain) {
                if(l.success)
                    t += l.build_ms;
                fs::remove(l.file_path);
                fs::remove(l.pch_path);
            }
            totals.push_back(t);
        }

        std::sort(totals.begin(), totals.end());
        double median = totals[totals.size() / 2];
        double mean = std::accumulate(totals.begin(), totals.end(), 0.0) / totals.size();
        std::println(
            "  Total chain build - Min: {:.1f}ms  Median: {:.1f}ms  Mean: {:.1f}ms  Max: {:.1f}ms",
            totals.front(),
            median,
            mean,
            totals.back());
    }

    // Cleanup first run.
    for(auto& l: links) {
        fs::remove(l.file_path);
        fs::remove(l.pch_path);
    }
}

static void bench_incremental(const std::vector<std::string>& headers,
                              std::size_t base_count,
                              int runs,
                              const std::string& resource_dir) {
    std::println("\n=== INCREMENTAL REBUILD (add 1 header to {} existing) ===", base_count);

    // Build base chain.
    std::string prev_pch;
    std::uint32_t prev_bound = 0;
    std::vector<std::string> temp_files;

    for(std::size_t i = 0; i < base_count && i < headers.size(); ++i) {
        std::string text = "#include <" + headers[i] + ">\n";
        std::string fp = temp_path("incr-base", "h");
        std::string pp = temp_path("incr-base", "pch");
        temp_files.push_back(fp);
        temp_files.push_back(pp);

        auto r = build_one_pch(text, fp, pp, resource_dir, prev_pch, prev_bound);
        if(!r.success) {
            std::println("  Base chain failed at link {} (<{}>)", i + 1, headers[i]);
            goto cleanup;
        }
        prev_pch = pp;
        prev_bound = 0;  // Chained: previous PCH doesn't cover current file.
    }

    {
        // Monolithic rebuild for comparison.
        std::string mono_preamble = make_preamble(headers, base_count + 1);
        std::string mono_file = temp_path("incr-mono", "h");
        std::string mono_pch = temp_path("incr-mono", "pch");
        temp_files.push_back(mono_file);
        temp_files.push_back(mono_pch);

        std::vector<double> mono_times, chain_times;

        for(int r = 0; r < runs; ++r) {
            fs::remove(mono_pch);
            auto mr = build_one_pch(mono_preamble, mono_file, mono_pch, resource_dir);
            if(mr.success)
                mono_times.push_back(mr.ms);
        }

        // Chained incremental: just append one more link.
        std::string extra_hdr = (base_count < headers.size()) ? headers[base_count] : "version";
        std::string extra_text = "#include <" + extra_hdr + ">\n";
        std::string extra_file = temp_path("incr-extra", "h");
        std::string extra_pch = temp_path("incr-extra", "pch");
        temp_files.push_back(extra_file);
        temp_files.push_back(extra_pch);

        for(int r = 0; r < runs; ++r) {
            fs::remove(extra_pch);
            auto cr = build_one_pch(extra_text,
                                    extra_file,
                                    extra_pch,
                                    resource_dir,
                                    prev_pch,
                                    prev_bound);
            if(cr.success)
                chain_times.push_back(cr.ms);
        }

        if(!mono_times.empty() && !chain_times.empty()) {
            std::sort(mono_times.begin(), mono_times.end());
            std::sort(chain_times.begin(), chain_times.end());
            double mono_med = mono_times[mono_times.size() / 2];
            double chain_med = chain_times[chain_times.size() / 2];
            std::println("  Monolithic full rebuild:  median {:.1f}ms", mono_med);
            std::println("  Chained append-one-link:  median {:.1f}ms", chain_med);
            std::println("  Speedup: {:.1f}x", mono_med / chain_med);
        }
    }

cleanup:
    for(auto& f: temp_files)
        fs::remove(f);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    int runs = 3;
    std::size_t chain_length = ALL_HEADERS.size();

    // Simple arg parsing.
    for(int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if(arg == "--runs" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if(arg == "--chain-length" && i + 1 < argc) {
            chain_length = std::atoi(argv[++i]);
            if(chain_length > ALL_HEADERS.size())
                chain_length = ALL_HEADERS.size();
        } else if(arg == "-h" || arg == "--help") {
            std::println("Usage: pch_chain_benchmark [--runs N] [--chain-length N]");
            return 0;
        }
    }

    // Find resource dir from argv[0].
    std::string resource_dir;
    {
        llvm::SmallString<256> bin_path(argv[0]);
        llvm::sys::fs::make_absolute(bin_path);
        auto bin_dir = llvm::sys::path::parent_path(bin_path);
        auto build_dir = llvm::sys::path::parent_path(bin_dir);
        llvm::SmallString<256> rd(build_dir);
        llvm::sys::path::append(rd, "lib", "clang", "21");
        if(llvm::sys::fs::exists(rd)) {
            resource_dir = std::string(rd);
        } else {
            // Try version-agnostic search.
            llvm::SmallString<256> lib_dir(build_dir);
            llvm::sys::path::append(lib_dir, "lib", "clang");
            std::error_code ec;
            for(llvm::sys::fs::directory_iterator it(lib_dir, ec), end; it != end;
                it.increment(ec)) {
                auto p = it->path();
                llvm::SmallString<256> candidate(p);
                llvm::sys::path::append(candidate, "include");
                if(llvm::sys::fs::exists(candidate)) {
                    resource_dir = std::string(p);
                    break;
                }
            }
        }
    }

    if(resource_dir.empty()) {
        std::println(stderr, "Cannot find clang resource dir. Run from build/bin/.");
        return 1;
    }

    std::println("PCH Chain Benchmark");
    std::println("  Resource dir: {}", resource_dir);
    std::println("  Chain length: {}", chain_length);
    std::println("  Runs: {}", runs);
    std::println("");

    bench_monolithic(ALL_HEADERS, chain_length, runs, resource_dir);
    bench_chained(ALL_HEADERS, chain_length, runs, resource_dir);
    bench_incremental(ALL_HEADERS, chain_length - 1, runs, resource_dir);

    return 0;
}
