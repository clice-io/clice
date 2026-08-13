/// Benchmark: monolithic PCH vs chained PCH using clice's compile() API.
///
/// Ported from PR #405. Answers the strategy question behind incremental
/// preamble builds: what does a one-link-per-#include PCH chain cost to
/// build (full and incremental) and to consume (AST load latency), against
/// the monolithic preamble the server builds today?
///
/// Tests:
///   1. Correctness of chained PCH across the C++ standard library headers
///   2. Build time: monolithic (single PCH) vs chained (one per #include)
///   3. Incremental rebuild: appending one header to an existing chain
///   4. Compile-with-PCH latency for both strategies
///   5. End-to-end: monolithic first, background split, incremental append
///
/// Usage:
///   pch_chain_benchmark [--runs N] [--chain-length N]

#include <algorithm>
#include <chrono>
#include <numeric>
#include <print>
#include <sstream>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/command.h"
#include "compile/compilation.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace clice;
using Clock = std::chrono::steady_clock;

namespace {

struct BenchmarkOptions {
    DecoKV(names = {"--runs"}; help = "Repetitions per measurement"; required = false;)
    <int> runs = 3;

    DecoKV(names = {"--chain-length"}; help = "Number of headers in the chain (capped at all)";
           required = false;)
    <int> chain_length = 0;

    DecoKV(names = {"--log-level"}; help = "Log level: trace, debug, info, warn, error, off";
           required = false;)
    <std::string> log_level = "off";

    DecoFlag(names = {"-h", "--help"}; help = "Show help message"; required = false;)
    help;
};

/// C++20 standard library headers in a dependency-friendly include order
/// (basic types first, I/O last).
const std::vector<std::string> ALL_HEADERS = {
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

/// Generate preamble text for the first `count` headers.
std::string make_preamble(const std::vector<std::string>& headers, std::size_t count) {
    std::string text;
    for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
        text += "#include <" + headers[i] + ">\n";
    }
    return text;
}

/// Owns every temporary path a benchmark routine creates and removes them
/// on scope exit, so failure paths can simply return.
struct TempTracker {
    std::vector<std::string> paths;

    ~TempTracker() {
        for(auto& path: paths) {
            fs::remove(path);
        }
    }

    std::string create(llvm::StringRef prefix, llvm::StringRef ext) {
        auto result = fs::createTemporaryFile(prefix, ext);
        if(!result) {
            std::println(stderr, "Failed to create temp file");
            std::exit(1);
        }
        paths.push_back(*result);
        return *result;
    }
};

/// A path inside the system temp dir for a buffer that is only ever
/// remapped, never written.
std::string virtual_path(llvm::StringRef name) {
    llvm::SmallString<128> path;
    llvm::sys::path::system_temp_directory(/*ErasedOnReboot=*/true, path);
    llvm::sys::path::append(path, name);
    return path.str().str();
}

/// Owns argument storage for one compile invocation.
struct ArgList {
    std::vector<std::string> storage;
    std::vector<const char*> argv;

    ArgList(std::initializer_list<std::string> args) : storage(args) {
        for(auto& arg: storage) {
            argv.push_back(arg.c_str());
        }
    }
};

ArgList make_pch_args(const std::string& file) {
    return ArgList{"clang++",
                   "-std=c++20",
                   "-resource-dir",
                   resource_dir().str(),
                   "-x",
                   "c++-header",
                   file};
}

ArgList make_source_args(const std::string& file) {
    return ArgList{"clang++",
                   "-std=c++20",
                   "-resource-dir",
                   resource_dir().str(),
                   "-fsyntax-only",
                   file};
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

struct PCHBuildResult {
    bool success = false;
    std::string path;
    double ms = 0;
    std::uint64_t size_bytes = 0;
    std::string error;
};

/// Build a single PCH (monolithic, or one chain link over `prev_pch`).
PCHBuildResult build_one_pch(const std::string& header_text,
                             const std::string& file_path,
                             const std::string& output_path,
                             const std::string& prev_pch = "",
                             std::uint32_t prev_bound = 0) {
    PCHBuildResult result;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    cp.output_file = output_path;

    auto args = make_pch_args(file_path);
    cp.arguments = args.argv;
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

    if(!ok) {
        auto errors = collect_errors(unit);
        result.error = errors.empty() ? "PCH compilation failed (no diagnostics)" : errors;
        return result;
    }

    // Flush to disk by destroying the unit.
    unit = CompilationUnit(nullptr);
    if(!llvm::sys::fs::exists(output_path)) {
        result.error = "PCH file not written to disk";
        return result;
    }

    result.success = true;
    result.path = output_path;
    if(auto error = llvm::sys::fs::file_size(output_path, result.size_bytes)) {
        result.size_bytes = 0;
    }
    return result;
}

/// Verify a PCH works by syntax-checking a test file against it.
bool verify_pch(const std::string& pch_path, std::uint32_t preamble_bound) {
    CompilationParams cp;
    cp.kind = CompilationKind::Content;

    auto file = virtual_path("pch-verify.cpp");
    auto args = make_source_args(file);
    cp.arguments = args.argv;
    cp.add_remapped_file(file, "static_assert(sizeof(int) > 0);\nint main() { return 0; }\n");
    cp.pch = {pch_path, preamble_bound};

    auto unit = compile(cp);
    return unit.completed();
}

/// Compile a source file over a given PCH and report the wall-clock time,
/// or -1 on failure.
double compile_with_pch(const std::string& source_text,
                        const std::string& source_file,
                        const std::string& pch_path,
                        std::uint32_t preamble_bound) {
    CompilationParams cp;
    cp.kind = CompilationKind::Content;

    auto args = make_source_args(source_file);
    cp.arguments = args.argv;
    cp.add_remapped_file(source_file, source_text);
    cp.pch = {pch_path, preamble_bound};

    auto start = Clock::now();
    auto unit = compile(cp);
    bool ok = unit.completed();
    unit = CompilationUnit(nullptr);
    auto end = Clock::now();

    if(!ok)
        return -1.0;
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double median_of(std::vector<double>& values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
}

void bench_monolithic(const std::vector<std::string>& headers, std::size_t count, int runs) {
    std::println("=== MONOLITHIC PCH ({} headers, {} runs) ===", count, runs);

    TempTracker temps;
    std::string preamble = make_preamble(headers, count);
    std::string file_path = temps.create("mono-preamble", "h");
    std::string pch_path = temps.create("mono", "pch");

    std::vector<double> times;
    times.reserve(runs);

    for(int r = 0; r < runs; r += 1) {
        fs::remove(pch_path);
        auto result = build_one_pch(preamble, file_path, pch_path);
        if(!result.success) {
            std::println(stderr, "  Run {}: FAILED - {}", r + 1, result.error);
            break;
        }
        times.push_back(result.ms);
        if(r == 0) {
            std::println("  Size: {} KB", result.size_bytes / 1024);
            auto bound = static_cast<std::uint32_t>(preamble.size());
            std::println("  Correctness: {}", verify_pch(pch_path, bound) ? "PASS" : "FAIL");
        }
    }

    if(!times.empty()) {
        double mean =
            std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
        double median = median_of(times);
        std::println("  Min: {:.1f}ms  Median: {:.1f}ms  Mean: {:.1f}ms  Max: {:.1f}ms",
                     times.front(),
                     median,
                     mean,
                     times.back());
    }
}

void bench_chained(const std::vector<std::string>& headers, std::size_t count, int runs) {
    std::println("\n=== CHAINED PCH ({} headers, {} runs) ===", count, runs);

    struct LinkInfo {
        std::string header;
        std::string pch_path;
        double build_ms = 0;
        bool success = false;
    };

    TempTracker temps;

    auto build_chain = [&](bool verbose) -> std::vector<LinkInfo> {
        std::vector<LinkInfo> links;
        links.reserve(count);

        std::string prev_pch;
        for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
            LinkInfo link;
            link.header = headers[i];

            std::string text = "#include <" + headers[i] + ">\n";
            std::string file_path = temps.create("chain-link", "h");
            link.pch_path = temps.create("chain", "pch");

            // For chained PCH the bound is always 0: PrecompiledPreambleBytes
            // tells clang to skip the first N bytes of the current source,
            // which is only correct when the PCH was built FROM the current
            // file (monolithic preamble). Each chain link is a separate
            // file, so nothing must be skipped.
            auto result = build_one_pch(text, file_path, link.pch_path, prev_pch, 0);
            link.build_ms = result.ms;
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

            // A failed link is skipped; the chain continues over the
            // previous PCH.
            if(result.success) {
                prev_pch = link.pch_path;
            }
            links.push_back(std::move(link));
        }

        return links;
    };

    // First run: verbose, report per-link times.
    auto links = build_chain(true);

    std::size_t passed = 0, failed = 0;
    double total_ms = 0;
    for(auto& link: links) {
        if(link.success) {
            passed += 1;
            total_ms += link.build_ms;
        } else {
            failed += 1;
        }
    }

    std::println("\n  Chain result: {} passed, {} failed, total {:.1f}ms",
                 passed,
                 failed,
                 total_ms);

    if(!links.empty() && links.back().success) {
        // bound=0: the chain PCH does not cover the verify file.
        std::println("  Final PCH correctness: {}",
                     verify_pch(links.back().pch_path, 0) ? "PASS" : "FAIL");
    }

    // Additional runs for timing statistics.
    if(runs > 1) {
        std::vector<double> totals;
        totals.push_back(total_ms);

        for(int r = 1; r < runs; r += 1) {
            auto chain = build_chain(false);
            double total = 0;
            for(auto& link: chain) {
                if(link.success)
                    total += link.build_ms;
            }
            totals.push_back(total);
        }

        double mean =
            std::accumulate(totals.begin(), totals.end(), 0.0) / static_cast<double>(totals.size());
        double median = median_of(totals);
        std::println(
            "  Total chain build - Min: {:.1f}ms  Median: {:.1f}ms  Mean: {:.1f}ms  Max: {:.1f}ms",
            totals.front(),
            median,
            mean,
            totals.back());
    }
}

void bench_incremental(const std::vector<std::string>& headers, std::size_t base_count, int runs) {
    std::println("\n=== INCREMENTAL REBUILD (add 1 header to {} existing) ===", base_count);

    TempTracker temps;

    // Build base chain.
    std::string prev_pch;
    for(std::size_t i = 0; i < base_count && i < headers.size(); i += 1) {
        std::string text = "#include <" + headers[i] + ">\n";
        auto result = build_one_pch(text,
                                    temps.create("incr-base", "h"),
                                    temps.create("incr-base", "pch"),
                                    prev_pch,
                                    0);
        if(!result.success) {
            std::println("  Base chain failed at link {} (<{}>)", i + 1, headers[i]);
            return;
        }
        prev_pch = result.path;
    }

    // Monolithic rebuild for comparison.
    std::string mono_preamble = make_preamble(headers, base_count + 1);
    std::string mono_file = temps.create("incr-mono", "h");
    std::string mono_pch = temps.create("incr-mono", "pch");

    std::vector<double> mono_times, chain_times;

    for(int r = 0; r < runs; r += 1) {
        fs::remove(mono_pch);
        auto result = build_one_pch(mono_preamble, mono_file, mono_pch);
        if(result.success)
            mono_times.push_back(result.ms);
    }

    // Chained incremental: just append one more link.
    std::string extra_hdr = (base_count < headers.size()) ? headers[base_count] : "version";
    std::string extra_text = "#include <" + extra_hdr + ">\n";
    std::string extra_file = temps.create("incr-extra", "h");
    std::string extra_pch = temps.create("incr-extra", "pch");

    for(int r = 0; r < runs; r += 1) {
        fs::remove(extra_pch);
        auto result = build_one_pch(extra_text, extra_file, extra_pch, prev_pch, 0);
        if(result.success)
            chain_times.push_back(result.ms);
    }

    if(!mono_times.empty() && !chain_times.empty()) {
        double mono_med = median_of(mono_times);
        double chain_med = median_of(chain_times);
        std::println("  Monolithic full rebuild:  median {:.1f}ms", mono_med);
        std::println("  Chained append-one-link:  median {:.1f}ms", chain_med);
        std::println("  Speedup: {:.1f}x", mono_med / chain_med);
    }
}

/// Sources for the AST load scenarios: light touches a couple of types
/// (lazy PCH load, best case); heavy references symbols from as many
/// headers as possible to force maximum PCH deserialization (worst case).
std::string make_light_source(const std::string& preamble) {
    return preamble + R"cpp(
int main() {
    std::vector<int> v = {1, 2, 3};
    return v[0];
}
)cpp";
}

std::string make_heavy_source(const std::string& preamble) {
    return preamble + R"cpp(
template <typename... Ts> void use(Ts&&...) {}

int main() {
    // <cstddef> <cstdint> <climits> <cfloat>
    std::size_t sz = 0; std::uint64_t u64 = 0;

    // <type_traits> <concepts> <compare>
    static_assert(std::is_integral_v<int>);
    static_assert(std::integral<int>);
    std::strong_ordering cmp = 1 <=> 2;

    // <initializer_list> <utility> <tuple> <optional> <variant> <any> <expected>
    auto il = {1, 2, 3};
    auto pr = std::make_pair(1, 2);
    auto tp = std::make_tuple(1, "hello", 3.14);
    std::optional<int> opt = 42;
    std::variant<int, double, std::string> var = "hello";
    std::any a = 42;

    // <bitset> <bit> <string_view> <string> <charconv> <format>
    std::bitset<64> bs(0xFF);
    auto pc = std::popcount(42u);
    std::string_view sv = "hello";
    std::string s = "world";
    auto fmt = std::format("{} {}", s, 42);

    // <array> <vector> <deque> <list> <forward_list>
    std::array<int, 3> arr = {1, 2, 3};
    std::vector<std::string> vec = {"a", "b"};
    std::deque<int> dq = {1, 2};
    std::list<int> lst = {1, 2};
    std::forward_list<int> fl = {1, 2};

    // <set> <map> <unordered_set> <unordered_map>
    std::set<int> st = {1, 2, 3};
    std::map<std::string, int> mp = {{"a", 1}};
    std::unordered_set<int> us = {1, 2};
    std::unordered_map<std::string, int> um = {{"b", 2}};

    // <stack> <queue> <span>
    std::stack<int> stk;
    std::queue<int> que;
    std::span<const int> spn(arr);

    // <iterator> <ranges> <algorithm> <numeric>
    auto it = vec.begin();
    auto rng = vec | std::views::take(1);
    std::sort(vec.begin(), vec.end());
    auto sum = std::accumulate(arr.begin(), arr.end(), 0);

    // <memory> <memory_resource> <scoped_allocator> <functional>
    auto up = std::make_unique<int>(42);
    auto sp = std::make_shared<std::string>("test");
    std::function<int(int)> fn = [](int x) { return x * 2; };

    // <ratio> <chrono>
    using half = std::ratio<1, 2>;
    auto now = std::chrono::system_clock::now();

    // <exception> <stdexcept> <system_error>
    try { throw std::runtime_error("test"); } catch(...) {}
    auto ec = std::make_error_code(std::errc::invalid_argument);

    // <typeinfo> <typeindex> <source_location>
    auto& ti = typeid(int);
    std::type_index tidx(ti);
    auto loc = std::source_location::current();

    // <new> <limits> <numbers> <valarray> <complex> <random>
    static_assert(std::numeric_limits<double>::is_iec559);
    constexpr auto pi = std::numbers::pi;
    std::valarray<double> va = {1.0, 2.0, 3.0};
    std::complex<double> cx(1.0, 2.0);
    std::mt19937 rng_eng(42);

    // <iosfwd> <ios> <streambuf> <istream> <ostream> <iostream> <sstream> <fstream>
    std::stringstream ss;
    ss << "hello " << 42;
    std::cout << ss.str() << std::endl;

    // <cmath> <cstdio> <cstdlib> <cstring> <ctime> <cassert> <cerrno>
    auto sq = std::sqrt(2.0);
    auto len = std::strlen("hello");
    auto t = std::time(nullptr);
    assert(sq > 1.0);

    // <atomic> <mutex> <condition_variable> <thread> <future>
    std::atomic<int> ai{0};
    std::mutex mtx;
    std::condition_variable cv;

    // <semaphore> <latch> <barrier> <stop_token> <shared_mutex>
    std::counting_semaphore<1> sem(1);
    std::latch lat(1);

    // <regex> <filesystem>
    std::regex rx("hello.*");
    auto cwd = std::filesystem::current_path();

    // <locale> <codecvt>
    auto& loc2 = std::locale::classic();

    use(sz, u64, cmp, il, pr, tp, opt, var, a, bs, pc, sv, s, fmt,
        arr, vec, dq, lst, fl, st, mp, us, um, stk, que, spn,
        it, rng, sum, up, sp, fn, now, ec, ti, tidx, loc,
        pi, va, cx, rng_eng, ss, sq, len, t, ai, mtx, cv,
        sem, lat, rx, cwd, loc2);
    return 0;
}
)cpp";
}

void bench_ast_load(const std::vector<std::string>& headers, std::size_t count, int runs) {
    std::println("\n=== AST LOAD LATENCY ({} headers, {} runs) ===", count, runs);

    auto preamble = make_preamble(headers, count);
    auto preamble_bound = static_cast<std::uint32_t>(preamble.size());

    TempTracker temps;

    // Build monolithic PCH.
    std::string mono_hdr = temps.create("ast-mono", "h");
    std::string mono_pch = temps.create("ast-mono", "pch");
    auto mono_result = build_one_pch(preamble, mono_hdr, mono_pch);
    if(!mono_result.success) {
        std::println("  Monolithic PCH build failed");
        return;
    }

    // Build chained PCH.
    std::string prev_pch;
    for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
        std::string text = "#include <" + headers[i] + ">\n";
        auto result = build_one_pch(text,
                                    temps.create("ast-chain", "h"),
                                    temps.create("ast-chain", "pch"),
                                    prev_pch,
                                    0);
        if(!result.success) {
            std::println("  Chain build failed at link {} (<{}>): {}",
                         i + 1,
                         headers[i],
                         result.error);
            return;
        }
        prev_pch = result.path;
    }
    std::string chain_pch = prev_pch;

    struct Scenario {
        const char* name;
        std::string source;
    };

    Scenario scenarios[] = {
        {"light (3 types)",     make_light_source(preamble)},
        {"heavy (all headers)", make_heavy_source(preamble)},
    };

    for(auto& [name, source]: scenarios) {
        std::println("\n  --- {} ---", name);
        auto source_file = virtual_path("ast-load-test.cpp");

        std::vector<double> mono_times, chain_times;

        for(int r = 0; r < runs; r += 1) {
            double ms = compile_with_pch(source, source_file, mono_pch, preamble_bound);
            if(ms >= 0)
                mono_times.push_back(ms);
        }

        for(int r = 0; r < runs; r += 1) {
            double ms = compile_with_pch(source, source_file, chain_pch, preamble_bound);
            if(ms >= 0)
                chain_times.push_back(ms);
        }

        if(!mono_times.empty()) {
            double med = median_of(mono_times);
            std::println("  Monolithic PCH → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         mono_times.front(),
                         mono_times.back());
        }
        if(!chain_times.empty()) {
            double med = median_of(chain_times);
            std::println("  Chained PCH    → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         chain_times.front(),
                         chain_times.back());
        }
        if(!mono_times.empty() && !chain_times.empty()) {
            double ratio = median_of(chain_times) / median_of(mono_times);
            std::println("  Ratio (chained/mono): {:.2f}x", ratio);
        } else if(chain_times.empty()) {
            std::println("  Chained PCH: compilation FAILED (heavy source may have errors)");
        }
    }
}

void bench_end_to_end(const std::vector<std::string>& headers, std::size_t count, int runs) {
    std::println("\n=== END-TO-END: monolithic → background split → incremental ===");
    if(count < 2) {
        std::println("  Need at least 2 headers");
        return;
    }

    TempTracker temps;

    // Phase 1: user opens a file — build the monolithic PCH they wait for.
    std::println("\n  Phase 1: Monolithic PCH (what the user waits for)");
    std::string mono_preamble = make_preamble(headers, count);
    std::string mono_hdr = temps.create("e2e-mono", "h");
    std::string mono_pch = temps.create("e2e-mono", "pch");

    auto mono_result = build_one_pch(mono_preamble, mono_hdr, mono_pch);
    if(!mono_result.success) {
        std::println("    FAILED: {}", mono_result.error);
        return;
    }
    std::println("    Build: {:.1f}ms", mono_result.ms);
    std::println("    Verify: {}",
                 verify_pch(mono_pch, static_cast<std::uint32_t>(mono_preamble.size())) ? "PASS"
                                                                                        : "FAIL");

    // Phase 2: background — split into a chain for future incremental use;
    // the user keeps editing and never waits for this.
    std::println("\n  Phase 2: Background chain split (async, user doesn't wait)");
    std::string prev_pch;
    std::vector<std::string> chain_pchs;

    auto split_start = Clock::now();
    for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
        std::string text = "#include <" + headers[i] + ">\n";
        auto result = build_one_pch(text,
                                    temps.create("e2e-chain", "h"),
                                    temps.create("e2e-chain", "pch"),
                                    prev_pch,
                                    0);
        if(!result.success) {
            std::println("    Chain failed at link {} (<{}>): {}", i + 1, headers[i], result.error);
            return;
        }
        prev_pch = result.path;
        chain_pchs.push_back(result.path);
    }
    auto split_end = Clock::now();
    double split_ms = std::chrono::duration<double, std::milli>(split_end - split_start).count();

    std::println("    Split into {} links: {:.1f}ms", chain_pchs.size(), split_ms);
    std::println("    Verify final link: {}", verify_pch(chain_pchs.back(), 0) ? "PASS" : "FAIL");

    // Phase 3: user adds a new #include at the preamble end.
    std::println("\n  Phase 3: User adds #include <chrono> at preamble end");
    std::string extra_text = "#include <chrono>\n";

    // Strategy A: monolithic — full rebuild.
    std::vector<double> mono_rebuild_times;
    for(int r = 0; r < runs; r += 1) {
        auto result = build_one_pch(mono_preamble + extra_text,
                                    temps.create("e2e-rebuild", "h"),
                                    temps.create("e2e-rebuild", "pch"));
        if(result.success)
            mono_rebuild_times.push_back(result.ms);
    }

    // Strategy B: chained — append one link to the cached chain.
    std::vector<double> chain_append_times;
    for(int r = 0; r < runs; r += 1) {
        auto result = build_one_pch(extra_text,
                                    temps.create("e2e-append", "h"),
                                    temps.create("e2e-append", "pch"),
                                    chain_pchs.back(),
                                    0);
        if(result.success)
            chain_append_times.push_back(result.ms);
    }

    if(!mono_rebuild_times.empty()) {
        std::println("    Monolithic full rebuild:  median {:.1f}ms",
                     median_of(mono_rebuild_times));
    }
    if(!chain_append_times.empty()) {
        double chain_med = median_of(chain_append_times);
        std::println("    Chained append 1 link:   median {:.1f}ms", chain_med);
        if(!mono_rebuild_times.empty()) {
            std::println("    Speedup: {:.0f}x", median_of(mono_rebuild_times) / chain_med);
        }
    }

    // Phase 4: verify the appended chain PCH works with real code.
    std::println("\n  Phase 4: Correctness — compile real code with appended chain");
    std::string verify_source = R"cpp(
int main() {
    auto now = std::chrono::system_clock::now();
    std::vector<std::string> v = {"hello"};
    std::map<int, double> m = {{1, 3.14}};
    return 0;
}
)cpp";
    std::string full_preamble = mono_preamble + extra_text;
    auto full_bound = static_cast<std::uint32_t>(full_preamble.size());
    std::string full_source = full_preamble + verify_source;
    auto source_file = virtual_path("e2e-verify.cpp");

    {
        auto result = build_one_pch(extra_text,
                                    temps.create("e2e-final", "h"),
                                    temps.create("e2e-final", "pch"),
                                    chain_pchs.back(),
                                    0);
        if(result.success) {
            // bound=0: the chain PCH covers no bytes of the source file.
            double ms = compile_with_pch(full_source, source_file, result.path, 0);
            std::println("    Compile with appended chain PCH: {}",
                         ms >= 0 ? std::format("PASS ({:.0f}ms)", ms) : "FAIL");
        } else {
            std::println("    Build appended chain: FAILED");
        }
    }

    {
        auto result = build_one_pch(full_preamble,
                                    temps.create("e2e-vfull", "h"),
                                    temps.create("e2e-vfull", "pch"));
        if(result.success) {
            double ms = compile_with_pch(full_source, source_file, result.path, full_bound);
            std::println("    Compile with monolithic PCH:     {}",
                         ms >= 0 ? std::format("PASS ({:.0f}ms)", ms) : "FAIL");
        }
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

    if(opts.help.value_or(false)) {
        std::ostringstream oss;
        kota::deco::cli::write_usage_for<BenchmarkOptions>(oss, "pch_chain_benchmark [OPTIONS]");
        std::print("{}", oss.str());
        return 0;
    }

    auto level = spdlog::level::from_str(*opts.log_level);
    clice::logging::options.level = level;
    clice::logging::stderr_logger("pch_chain_benchmark", clice::logging::options);

    int runs = *opts.runs;
    if(runs <= 0) {
        std::println(stderr, "Error: --runs must be positive (got {})", runs);
        return 1;
    }

    std::size_t chain_length = ALL_HEADERS.size();
    if(*opts.chain_length > 0) {
        chain_length = std::min(static_cast<std::size_t>(*opts.chain_length), chain_length);
    }
    if(chain_length < 2) {
        std::println(stderr, "Error: --chain-length must be at least 2");
        return 1;
    }

    if(resource_dir().empty()) {
        std::println(stderr, "Cannot find the clang resource dir next to this binary.");
        return 1;
    }

    std::println("PCH Chain Benchmark");
    std::println("  Resource dir: {}", resource_dir());
    std::println("  Chain length: {}", chain_length);
    std::println("  Runs: {}", runs);
    std::println("");

    bench_monolithic(ALL_HEADERS, chain_length, runs);
    bench_chained(ALL_HEADERS, chain_length, runs);
    bench_incremental(ALL_HEADERS, chain_length - 1, runs);
    bench_ast_load(ALL_HEADERS, chain_length, runs);
    bench_end_to_end(ALL_HEADERS, chain_length, runs);

    return 0;
}
