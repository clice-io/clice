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
// AST load latency: compile a source file using monolithic vs chained PCH
// ---------------------------------------------------------------------------

/// Compile a source file with a given PCH and measure the time.
static double compile_with_pch(const std::string& source_text,
                               const std::string& source_file,
                               const std::string& pch_path,
                               std::uint32_t preamble_bound,
                               const std::string& resource_dir) {
    CompilationParams cp;
    cp.kind = CompilationKind::Content;
    cp.directory = "/tmp";

    std::vector<std::string> arg_storage =
        {"clang++", "-std=c++20", "-resource-dir", resource_dir, "-fsyntax-only", source_file};
    std::vector<const char*> args;
    for(auto& s: arg_storage)
        args.push_back(s.c_str());
    cp.arguments = args;

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

static void bench_ast_load(const std::vector<std::string>& headers,
                           std::size_t count,
                           int runs,
                           const std::string& resource_dir) {
    std::println("\n=== AST LOAD LATENCY ({} headers, {} runs) ===", count, runs);

    // Light source: uses only a few types (lazy PCH load, best case).
    std::string light_source = make_preamble(headers, count) + R"cpp(
int main() {
    std::vector<int> v = {1, 2, 3};
    return v[0];
}
)cpp";

    // Heavy source: references symbols from as many headers as possible
    // to force maximum PCH deserialization (worst case).
    std::string heavy_source = make_preamble(headers, count) + R"cpp(
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
    auto preamble = make_preamble(headers, count);
    auto preamble_bound = static_cast<std::uint32_t>(preamble.size());

    // --- Build monolithic PCH ---
    std::string mono_preamble = make_preamble(headers, count);
    std::string mono_hdr = temp_path("ast-mono", "h");
    std::string mono_pch = temp_path("ast-mono", "pch");
    auto mono_r = build_one_pch(mono_preamble, mono_hdr, mono_pch, resource_dir);
    if(!mono_r.success) {
        std::println("  Monolithic PCH build failed");
        fs::remove(mono_hdr);
        return;
    }

    // --- Build chained PCH ---
    std::string prev_pch;
    std::vector<std::string> chain_temps;
    bool chain_ok = true;
    for(std::size_t i = 0; i < count && i < headers.size(); ++i) {
        std::string text = "#include <" + headers[i] + ">\n";
        std::string fp = temp_path("ast-chain", "h");
        std::string pp = temp_path("ast-chain", "pch");
        chain_temps.push_back(fp);
        chain_temps.push_back(pp);

        auto r = build_one_pch(text, fp, pp, resource_dir, prev_pch, 0);
        if(!r.success) {
            std::println("  Chain build failed at link {} (<{}>): {}", i + 1, headers[i], r.error);
            chain_ok = false;
            break;
        }
        prev_pch = pp;
    }

    if(!chain_ok) {
        fs::remove(mono_hdr);
        fs::remove(mono_pch);
        for(auto& f: chain_temps)
            fs::remove(f);
        return;
    }

    std::string chain_pch = prev_pch;

    // --- Run both scenarios: light and heavy ---
    struct Scenario {
        const char* name;
        std::string source;
    };

    Scenario scenarios[] = {
        {"light (3 types)",     light_source},
        {"heavy (all headers)", heavy_source},
    };

    for(auto& [name, source]: scenarios) {
        std::println("\n  --- {} ---", name);
        std::string source_file = "/tmp/ast-load-test.cpp";

        std::vector<double> mono_times, chain_times;

        for(int r = 0; r < runs; ++r) {
            double t =
                compile_with_pch(source, source_file, mono_pch, preamble_bound, resource_dir);
            if(t >= 0)
                mono_times.push_back(t);
        }

        for(int r = 0; r < runs; ++r) {
            double t =
                compile_with_pch(source, source_file, chain_pch, preamble_bound, resource_dir);
            if(t >= 0)
                chain_times.push_back(t);
        }

        if(!mono_times.empty()) {
            std::sort(mono_times.begin(), mono_times.end());
            double med = mono_times[mono_times.size() / 2];
            std::println("  Monolithic PCH → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         mono_times.front(),
                         mono_times.back());
        }
        if(!chain_times.empty()) {
            std::sort(chain_times.begin(), chain_times.end());
            double med = chain_times[chain_times.size() / 2];
            std::println("  Chained PCH    → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         chain_times.front(),
                         chain_times.back());
        }
        if(!mono_times.empty() && !chain_times.empty()) {
            double mono_med = mono_times[mono_times.size() / 2];
            double chain_med = chain_times[chain_times.size() / 2];
            double ratio = chain_med / mono_med;
            std::println("  Ratio (chained/mono): {:.2f}x", ratio);
        } else if(chain_times.empty()) {
            std::println("  Chained PCH: compilation FAILED (heavy source may have errors)");
        }
    }

    // Cleanup.
    fs::remove(mono_hdr);
    fs::remove(mono_pch);
    for(auto& f: chain_temps)
        fs::remove(f);
}

// ---------------------------------------------------------------------------
// End-to-end: monolithic first → background split → incremental append
// ---------------------------------------------------------------------------

static void bench_end_to_end(const std::vector<std::string>& headers,
                             std::size_t count,
                             int runs,
                             const std::string& resource_dir) {
    std::println("\n=== END-TO-END: monolithic → background split → incremental ===");
    if(count < 2) {
        std::println("  Need at least 2 headers");
        return;
    }

    std::vector<std::string> temp_files;
    auto track = [&](const std::string& p) {
        temp_files.push_back(p);
        return p;
    };

    // Phase 1: User opens file. Build monolithic PCH immediately.
    std::println("\n  Phase 1: Monolithic PCH (what user waits for)");
    std::string mono_preamble = make_preamble(headers, count);
    std::string mono_hdr = track(temp_path("e2e-mono", "h"));
    std::string mono_pch = track(temp_path("e2e-mono", "pch"));

    auto mono_start = Clock::now();
    auto mono_r = build_one_pch(mono_preamble, mono_hdr, mono_pch, resource_dir);
    auto mono_end = Clock::now();
    double mono_ms = std::chrono::duration<double, std::milli>(mono_end - mono_start).count();

    if(!mono_r.success) {
        std::println("    FAILED: {}", mono_r.error);
        goto e2e_cleanup;
    }
    std::println("    Build: {:.1f}ms", mono_ms);
    std::println(
        "    Verify: {}",
        verify_pch(mono_pch, static_cast<std::uint32_t>(mono_preamble.size()), resource_dir)
            ? "PASS"
            : "FAIL");

    {
        // Phase 2: Background — split into chain for future incremental use.
        // This happens while user is happily editing. They don't wait for this.
        std::println("\n  Phase 2: Background chain split (async, user doesn't wait)");
        std::string prev_pch;
        std::vector<std::string> chain_pchs;
        bool chain_ok = true;

        auto split_start = Clock::now();
        for(std::size_t i = 0; i < count && i < headers.size(); ++i) {
            std::string text = "#include <" + headers[i] + ">\n";
            std::string fp = track(temp_path("e2e-chain", "h"));
            std::string pp = track(temp_path("e2e-chain", "pch"));

            auto r = build_one_pch(text, fp, pp, resource_dir, prev_pch, 0);
            if(!r.success) {
                std::println("    Chain failed at link {} (<{}>): {}", i + 1, headers[i], r.error);
                chain_ok = false;
                break;
            }
            prev_pch = pp;
            chain_pchs.push_back(pp);
        }
        auto split_end = Clock::now();
        double split_ms =
            std::chrono::duration<double, std::milli>(split_end - split_start).count();

        if(!chain_ok)
            goto e2e_cleanup;

        std::println("    Split into {} links: {:.1f}ms", chain_pchs.size(), split_ms);
        std::println("    Verify final link: {}",
                     verify_pch(chain_pchs.back(), 0, resource_dir) ? "PASS" : "FAIL");

        // Phase 3: User adds a new #include at the end. Compare strategies.
        std::println("\n  Phase 3: User adds #include <chrono> at preamble end");

        std::string extra_header = "chrono";
        std::string extra_text = "#include <" + extra_header + ">\n";

        // Strategy A: Monolithic — full rebuild.
        std::vector<double> mono_rebuild_times;
        for(int r = 0; r < runs; ++r) {
            std::string new_preamble = mono_preamble + extra_text;
            std::string hdr = temp_path("e2e-rebuild", "h");
            std::string pch = temp_path("e2e-rebuild", "pch");
            temp_files.push_back(hdr);
            temp_files.push_back(pch);

            auto rb = build_one_pch(new_preamble, hdr, pch, resource_dir);
            if(rb.success)
                mono_rebuild_times.push_back(rb.ms);
        }

        // Strategy B: Chained — append one link from cached chain.
        std::vector<double> chain_append_times;
        for(int r = 0; r < runs; ++r) {
            std::string fp = temp_path("e2e-append", "h");
            std::string pp = temp_path("e2e-append", "pch");
            temp_files.push_back(fp);
            temp_files.push_back(pp);

            auto ap = build_one_pch(extra_text, fp, pp, resource_dir, chain_pchs.back(), 0);
            if(ap.success)
                chain_append_times.push_back(ap.ms);
        }

        // Report.
        if(!mono_rebuild_times.empty()) {
            std::sort(mono_rebuild_times.begin(), mono_rebuild_times.end());
            std::println("    Monolithic full rebuild:  median {:.1f}ms",
                         mono_rebuild_times[mono_rebuild_times.size() / 2]);
        }
        if(!chain_append_times.empty()) {
            std::sort(chain_append_times.begin(), chain_append_times.end());
            double chain_med = chain_append_times[chain_append_times.size() / 2];
            std::println("    Chained append 1 link:   median {:.1f}ms", chain_med);
            if(!mono_rebuild_times.empty()) {
                double mono_med = mono_rebuild_times[mono_rebuild_times.size() / 2];
                std::println("    Speedup: {:.0f}x", mono_med / chain_med);
            }
        }

        // Phase 4: Verify the appended chain PCH actually works with real code.
        std::println("\n  Phase 4: Correctness — compile real code with appended chain");
        std::string verify_source = R"cpp(
#include <chrono>
int main() {
    auto now = std::chrono::system_clock::now();
    std::vector<std::string> v = {"hello"};
    std::map<int, double> m = {{1, 3.14}};
    return 0;
}
)cpp";
        // Prepend the full preamble + chrono for source compilation.
        std::string full_preamble = mono_preamble + extra_text;
        std::uint32_t full_bound = static_cast<std::uint32_t>(full_preamble.size());
        std::string full_source = full_preamble + verify_source;
        std::string source_file = "/tmp/e2e-verify.cpp";

        // Use the last append PCH.
        std::string append_pch = temp_files[temp_files.size() - 1];  // last .pch
        // Find it: it's the last .pch in chain_append_times run
        // Actually, just build one more for the verify.
        {
            std::string fp = track(temp_path("e2e-final", "h"));
            std::string pp = track(temp_path("e2e-final", "pch"));
            auto ap = build_one_pch(extra_text, fp, pp, resource_dir, chain_pchs.back(), 0);
            if(ap.success) {
                // Compile source using the appended chain PCH.
                // bound=0 because the PCH doesn't cover any bytes of the source file.
                double t = compile_with_pch(full_source, source_file, pp, 0, resource_dir);
                std::println("    Compile with appended chain PCH: {}",
                             t >= 0 ? "PASS (" + std::to_string(int(t)) + "ms)" : "FAIL");
            } else {
                std::println("    Build appended chain: FAILED");
            }
        }

        // Also verify monolithic rebuild works.
        {
            std::string hdr = track(temp_path("e2e-vfull", "h"));
            std::string pch = track(temp_path("e2e-vfull", "pch"));
            auto rb = build_one_pch(full_preamble, hdr, pch, resource_dir);
            if(rb.success) {
                double t =
                    compile_with_pch(full_source, source_file, pch, full_bound, resource_dir);
                std::println("    Compile with monolithic PCH:     {}",
                             t >= 0 ? "PASS (" + std::to_string(int(t)) + "ms)" : "FAIL");
            }
        }
    }

e2e_cleanup:
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
    bench_ast_load(ALL_HEADERS, chain_length, runs, resource_dir);
    bench_end_to_end(ALL_HEADERS, chain_length, runs, resource_dir);

    return 0;
}
