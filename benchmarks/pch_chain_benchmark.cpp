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

#include "stats.h"
#include "command/argument_parser.h"
#include "command/command.h"
#include "compile/compilation.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/deco/deco.h"
#include "llvm/ADT/StringMap.h"
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
                             const std::string& prev_pch = "") {
    PCHBuildResult result;

    CompilationParams cp;
    cp.kind = CompilationKind::Preamble;
    cp.output_file = output_path;

    auto args = make_pch_args(file_path);
    cp.arguments = args.argv;
    cp.add_remapped_file(file_path, header_text);

    if(!prev_pch.empty()) {
        // Bound 0: PrecompiledPreambleBytes tells clang to skip the first N
        // bytes of the current source, which is only correct when the PCH
        // was built FROM the current file (monolithic preamble). Each chain
        // link is a separate file, so nothing must be skipped.
        cp.pch = {prev_pch, 0};
    }

    auto start = Clock::now();

    PCHInfo pch_info;
    auto unit = compile(cp, pch_info);
    bool ok = unit.completed();

    if(!ok) {
        result.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        auto errors = collect_errors(unit);
        result.error = errors.empty() ? "PCH compilation failed (no diagnostics)" : errors;
        return result;
    }

    // The unit's destructor flushes the PCH to disk; that write is part of
    // the build cost being compared, so destroy before stopping the clock.
    unit = CompilationUnit(nullptr);
    result.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();

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

std::string make_heavy_source(const std::string& preamble,
                              const std::vector<std::string>& headers,
                              std::size_t count);

/// Verify a PCH by compiling code that uses representative declarations
/// from every selected header WITHOUT re-including any of them (the heavy
/// source over an empty preamble), so a loadable but incomplete PCH — a
/// chain that dropped earlier links — fails instead of passing. Bound 0:
/// the verify source does not embed the PCH's preamble text, so no bytes
/// of it may be skipped.
bool verify_pch(const std::string& pch_path,
                const std::vector<std::string>& headers,
                std::size_t count) {
    CompilationParams cp;
    cp.kind = CompilationKind::Content;

    auto file = virtual_path("pch-verify.cpp");
    auto args = make_source_args(file);
    cp.arguments = args.argv;
    cp.add_remapped_file(file, make_heavy_source("", headers, count));
    cp.pch = {pch_path, 0};

    auto unit = compile(cp);
    // completed() only covers frontend execution; a compile over a broken
    // PCH still completes carrying ordinary error diagnostics.
    return unit.completed() && collect_errors(unit).empty();
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
    // See verify_pch: an erroneous parse still reports completed(), and its
    // timing must not enter the latency samples.
    bool ok = unit.completed() && collect_errors(unit).empty();
    unit = CompilationUnit(nullptr);
    auto end = Clock::now();

    if(!ok)
        return -1.0;
    return std::chrono::duration<double, std::milli>(end - start).count();
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
            std::println("  Correctness: {}",
                         verify_pch(pch_path, headers, count) ? "PASS" : "FAIL");
        }
    }

    if(!times.empty()) {
        std::ranges::sort(times);
        double mean =
            std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
        double median = bench::percentile(times, 0.5);
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
        std::string pch_path;
        double build_ms = 0;
        bool success = false;
    };

    auto build_chain = [&](TempTracker& temps, bool verbose) -> std::vector<LinkInfo> {
        std::vector<LinkInfo> links;
        links.reserve(count);

        std::string prev_pch;
        for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
            LinkInfo link;
            std::string text = "#include <" + headers[i] + ">\n";
            std::string file_path = temps.create("chain-link", "h");
            link.pch_path = temps.create("chain", "pch");

            auto result = build_one_pch(text, file_path, link.pch_path, prev_pch);
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

            links.push_back(std::move(link));
            // A partial chain must not masquerade as the requested one:
            // stop here and let the caller discard the run.
            if(!result.success) {
                break;
            }
            prev_pch = links.back().pch_path;
        }

        return links;
    };

    auto is_complete = [&](const std::vector<LinkInfo>& links) {
        return links.size() == count && links.back().success;
    };

    // First run: verbose, report per-link times. Its chain must survive
    // for the correctness check below.
    TempTracker temps;
    auto links = build_chain(temps, true);

    double total_ms = 0;
    for(auto& link: links) {
        if(link.success) {
            total_ms += link.build_ms;
        }
    }

    if(!is_complete(links)) {
        std::println("\n  Chain INCOMPLETE ({}/{} links built) — no timing statistics",
                     std::ranges::count_if(links, &LinkInfo::success),
                     count);
        return;
    }

    std::println("\n  Chain result: {} links, total {:.1f}ms", links.size(), total_ms);
    std::println("  Final PCH correctness: {}",
                 verify_pch(links.back().pch_path, headers, count) ? "PASS" : "FAIL");

    // Additional runs for timing statistics.
    if(runs > 1) {
        std::vector<double> totals;
        totals.push_back(total_ms);

        for(int r = 1; r < runs; r += 1) {
            // A tracker per run: only the summed time survives the
            // iteration, and a full standard-library chain of PCHs per run
            // would otherwise pile up in the temp dir until exit.
            TempTracker run_temps;
            auto chain = build_chain(run_temps, false);
            if(!is_complete(chain)) {
                std::println("  Run {}: chain incomplete, discarded", r + 1);
                continue;
            }
            double total = 0;
            for(auto& link: chain) {
                total += link.build_ms;
            }
            totals.push_back(total);
        }

        std::ranges::sort(totals);
        double mean =
            std::accumulate(totals.begin(), totals.end(), 0.0) / static_cast<double>(totals.size());
        double median = bench::percentile(totals, 0.5);
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
                                    prev_pch);
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
    std::string extra_text = "#include <" + headers[base_count] + ">\n";
    std::string extra_file = temps.create("incr-extra", "h");
    std::string extra_pch = temps.create("incr-extra", "pch");

    for(int r = 0; r < runs; r += 1) {
        fs::remove(extra_pch);
        auto result = build_one_pch(extra_text, extra_file, extra_pch, prev_pch);
        if(result.success)
            chain_times.push_back(result.ms);
    }

    if(!mono_times.empty() && !chain_times.empty()) {
        std::ranges::sort(mono_times);
        std::ranges::sort(chain_times);
        double mono_med = bench::percentile(mono_times, 0.5);
        double chain_med = bench::percentile(chain_times, 0.5);
        std::println("  Monolithic full rebuild:  median {:.1f}ms", mono_med);
        std::println("  Chained append-one-link:  median {:.1f}ms", chain_med);
        std::println("  Speedup: {:.1f}x", mono_med / chain_med);
    }
}

/// Sources for the AST load scenarios: light touches a couple of scalar
/// types (lazy PCH load, best case); heavy references symbols from every
/// selected header to force maximum PCH deserialization (worst case).
/// Both are built from the selected header prefix so that any accepted
/// --chain-length yields a source that compiles cleanly.
std::string make_light_source(const std::string& preamble) {
    // <cstddef> and <cstdint> lead ALL_HEADERS, and --chain-length is at
    // least 2, so these types exist for any prefix.
    return preamble + R"cpp(
int main() {
    std::size_t n = sizeof(std::uint64_t);
    return static_cast<int>(n);
}
)cpp";
}

/// One usage statement per header for the heavy source. Each snippet uses
/// declarations only from its own header and headers EARLIER in
/// ALL_HEADERS, so emitting the snippets of a contiguous prefix in header
/// order yields a valid program. Headers with nothing to use have no
/// entry (<iosfwd> and <codecvt> declare nothing to instantiate
/// standalone; <expected> is empty under -std=c++20).
const llvm::StringMap<llvm::StringRef> HEAVY_SNIPPETS = {
    {"cstddef",            "std::size_t sz = 0; use(sz);"                            },
    {"cstdint",            "std::uint64_t u64 = 0; use(u64);"                        },
    {"climits",            "use(INT_MAX);"                                           },
    {"cfloat",             "use(DBL_EPSILON);"                                       },
    {"type_traits",        "static_assert(std::is_integral_v<int>);"                 },
    {"concepts",           "static_assert(std::integral<int>);"                      },
    {"compare",            "std::strong_ordering cmp = 1 <=> 2; use(cmp);"           },
    {"initializer_list",   "auto il = {1, 2, 3}; use(il);"                           },
    {"utility",            "auto pr = std::make_pair(1, 2); use(pr);"                },
    {"tuple",              "auto tp = std::make_tuple(1, \"hello\", 3.14); use(tp);" },
    {"optional",           "std::optional<int> opt = 42; use(opt);"                  },
    {"variant",            "std::variant<int, double> var = 3.14; use(var);"         },
    {"any",                "std::any a = 42; use(a);"                                },
    {"bitset",             "std::bitset<64> bs(0xFF); use(bs);"                      },
    {"bit",                "auto pc = std::popcount(42u); use(pc);"                  },
    {"string_view",        "std::string_view sv = \"hello\"; use(sv);"               },
    {"string",             "std::string s = \"world\"; use(s);"                      },
    {"charconv",
     "char buf[16]; auto res = std::to_chars(buf, buf + 16, 42); "
     "use(res.ptr);"                                                                 },
    {"format",             "auto fmt = std::format(\"{} {}\", s, 42); use(fmt);"     },
    {"array",              "std::array<int, 3> arr = {1, 2, 3}; use(arr);"           },
    {"vector",             "std::vector<std::string> vec = {\"a\", \"b\"}; use(vec);"},
    {"deque",              "std::deque<int> dq = {1, 2}; use(dq);"                   },
    {"list",               "std::list<int> lst = {1, 2}; use(lst);"                  },
    {"forward_list",       "std::forward_list<int> fl = {1, 2}; use(fl);"            },
    {"set",                "std::set<int> st = {1, 2, 3}; use(st);"                  },
    {"map",                "std::map<std::string, int> mp = {{\"a\", 1}}; use(mp);"  },
    {"unordered_set",      "std::unordered_set<int> us = {1, 2}; use(us);"           },
    {"unordered_map",
     "std::unordered_map<std::string, int> um = {{\"b\", 2}}; "
     "use(um);"                                                                      },
    {"stack",              "std::stack<int> stk; use(stk);"                          },
    {"queue",              "std::queue<int> que; use(que);"                          },
    {"span",               "std::span<const int> spn(arr); use(spn);"                },
    {"iterator",           "use(std::distance(vec.begin(), vec.end()));"             },
    {"ranges",             "auto rng = vec | std::views::take(1); use(rng);"         },
    {"algorithm",          "std::sort(vec.begin(), vec.end());"                      },
    {"numeric",
     "auto sum = std::accumulate(arr.begin(), arr.end(), 0); "
     "use(sum);"                                                                     },
    {"memory",             "auto up = std::make_unique<int>(42); use(up);"           },
    {"memory_resource",    "std::pmr::monotonic_buffer_resource mbr; use(mbr);"      },
    {"scoped_allocator",
     "std::scoped_allocator_adaptor<std::allocator<int>> saa; "
     "use(saa);"                                                                     },
    {"functional",
     "std::function<int(int)> fn = [](int x) { return x * 2; }; "
     "use(fn);"                                                                      },
    {"ratio",              "using half = std::ratio<1, 2>; use(half::num);"          },
    {"chrono",             "auto now = std::chrono::system_clock::now(); use(now);"  },
    {"exception",          "use(std::uncaught_exceptions());"                        },
    {"stdexcept",          "std::runtime_error re(\"test\"); use(re);"               },
    {"system_error",
     "auto ec = std::make_error_code(std::errc::invalid_argument); "
     "use(ec);"                                                                      },
    {"typeinfo",           "auto& ti = typeid(int); use(ti);"                        },
    {"typeindex",          "std::type_index tidx(typeid(int)); use(tidx);"           },
    {"source_location",    "auto loc = std::source_location::current(); use(loc);"   },
    {"new",                "use(std::nothrow);"                                      },
    {"limits",             "static_assert(std::numeric_limits<double>::is_iec559);"  },
    {"numbers",            "constexpr auto pi = std::numbers::pi; use(pi);"          },
    {"valarray",           "std::valarray<double> va = {1.0, 2.0, 3.0}; use(va);"    },
    {"complex",            "std::complex<double> cx(1.0, 2.0); use(cx);"             },
    {"random",             "std::mt19937 rng_eng(42); use(rng_eng);"                 },
    {"ios",                "use(std::ios_base::app);"                                },
    {"streambuf",          "std::streambuf* sb = nullptr; use(sb);"                  },
    {"istream",            "std::istream* is = nullptr; use(is);"                    },
    {"ostream",            "std::ostream* os = nullptr; use(os);"                    },
    {"iostream",           "std::cout << 42;"                                        },
    {"sstream",            "std::stringstream ss; ss << \"hello\"; use(ss);"         },
    {"fstream",            "std::ifstream ifs; use(ifs);"                            },
    {"cmath",              "auto sq = std::sqrt(2.0); use(sq);"                      },
    {"cstdio",             "use(EOF);"                                               },
    {"cstdlib",            "use(std::abs(-1));"                                      },
    {"cstring",            "auto len = std::strlen(\"hello\"); use(len);"            },
    {"ctime",              "auto t = std::time(nullptr); use(t);"                    },
    {"cassert",            "assert(1 + 1 == 2);"                                     },
    {"cerrno",             "use(errno);"                                             },
    {"atomic",             "std::atomic<int> ai{0}; use(ai);"                        },
    {"mutex",              "std::mutex mtx; use(mtx);"                               },
    {"condition_variable", "std::condition_variable cv; use(cv);"                    },
    {"thread",             "use(std::this_thread::get_id());"                        },
    {"future",             "std::promise<int> prom; use(prom);"                      },
    {"semaphore",          "std::counting_semaphore<1> sem(1); use(sem);"            },
    {"latch",              "std::latch lat(1); use(lat);"                            },
    {"barrier",            "std::barrier bar(1); use(bar);"                          },
    {"stop_token",         "std::stop_source ssrc; use(ssrc);"                       },
    {"shared_mutex",       "std::shared_mutex smtx; use(smtx);"                      },
    {"regex",              "std::regex rx(\"hello.*\"); use(rx);"                    },
    {"filesystem",         "auto cwd = std::filesystem::current_path(); use(cwd);"   },
    {"locale",             "auto& lc = std::locale::classic(); use(lc);"             },
};

std::string make_heavy_source(const std::string& preamble,
                              const std::vector<std::string>& headers,
                              std::size_t count) {
    std::string source = preamble;
    source += "\ntemplate <typename... Ts> void use(Ts&&...) {}\n\nint main() {\n";
    for(std::size_t i = 0; i < count && i < headers.size(); i += 1) {
        auto it = HEAVY_SNIPPETS.find(headers[i]);
        if(it != HEAVY_SNIPPETS.end()) {
            source += "    ";
            source += it->second;
            source += "\n";
        }
    }
    source += "    return 0;\n}\n";
    return source;
}

void bench_ast_load(const std::vector<std::string>& headers, std::size_t count, int runs) {
    std::println("\n=== AST LOAD LATENCY ({} headers, {} runs) ===", count, runs);
    std::println("  (mono consumes the source with its preamble skipped via the bound;");
    std::println("   the chain re-preprocesses the include lines under the imported PCH)");

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
                                    prev_pch);
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
        std::string name;
        std::string source;
    };

    Scenario scenarios[] = {
        {"light (2 types)", make_light_source(preamble)},
        {std::format("heavy ({} headers)", count), make_heavy_source(preamble, headers, count)},
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
            // Bound 0: the chain PCH was built from separate per-header
            // files, so no bytes of this source may be skipped (see
            // build_one_pch). The chain therefore re-preprocesses the
            // include lines — guard-skipped, but not free — which the mono
            // side avoids entirely; the ratio includes that gap.
            double ms = compile_with_pch(source, source_file, chain_pch, 0);
            if(ms >= 0)
                chain_times.push_back(ms);
        }

        std::ranges::sort(mono_times);
        std::ranges::sort(chain_times);
        if(!mono_times.empty()) {
            double med = bench::percentile(mono_times, 0.5);
            std::println("  Monolithic PCH → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         mono_times.front(),
                         mono_times.back());
        }
        if(!chain_times.empty()) {
            double med = bench::percentile(chain_times, 0.5);
            std::println("  Chained PCH    → compile:  median {:.1f}ms  (min {:.1f}, max {:.1f})",
                         med,
                         chain_times.front(),
                         chain_times.back());
        }
        if(!mono_times.empty() && !chain_times.empty()) {
            double ratio = bench::percentile(chain_times, 0.5) / bench::percentile(mono_times, 0.5);
            std::println("  Ratio (chained/mono): {:.2f}x", ratio);
        } else if(chain_times.empty()) {
            std::println("  Chained PCH: compilation FAILED");
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
    std::println("    Verify: {}", verify_pch(mono_pch, headers, count) ? "PASS" : "FAIL");

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
                                    prev_pch);
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
    std::println("    Verify final link: {}",
                 verify_pch(chain_pchs.back(), headers, count) ? "PASS" : "FAIL");

    // Phase 3: user adds a new #include at the preamble end. <cinttypes>
    // is deliberately outside ALL_HEADERS: appending a header already in
    // the chain would measure a guarded-include no-op.
    std::println("\n  Phase 3: User adds #include <cinttypes> at preamble end");
    std::string extra_text = "#include <cinttypes>\n";

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
                                    chain_pchs.back());
        if(result.success)
            chain_append_times.push_back(result.ms);
    }

    std::ranges::sort(mono_rebuild_times);
    std::ranges::sort(chain_append_times);
    if(!mono_rebuild_times.empty()) {
        std::println("    Monolithic full rebuild:  median {:.1f}ms",
                     bench::percentile(mono_rebuild_times, 0.5));
    }
    if(!chain_append_times.empty()) {
        double chain_med = bench::percentile(chain_append_times, 0.5);
        std::println("    Chained append 1 link:   median {:.1f}ms", chain_med);
        if(!mono_rebuild_times.empty()) {
            std::println("    Speedup: {:.0f}x",
                         bench::percentile(mono_rebuild_times, 0.5) / chain_med);
        }
    }

    // Phase 4: verify the appended chain PCH works with real code. The
    // source only uses the appended header, so it stays valid for any
    // --chain-length.
    std::println("\n  Phase 4: Correctness — compile real code with appended chain");
    std::string verify_source = R"cpp(
int main() {
    std::intmax_t value = std::imaxabs(-42);
    return static_cast<int>(value - 42);
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
                                    chain_pchs.back());
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
