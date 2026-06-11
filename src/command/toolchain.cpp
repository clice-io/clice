#include "command/toolchain.h"

#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/command.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/meta/enum.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"

#ifndef _WIN32
#include <unistd.h>
extern char** environ;
#endif

namespace clice {

namespace {

namespace ranges = std::ranges;

#ifndef _WIN32
/// Process environment with LANG pinned to C, so driver output is not localized.
/// On Windows the env is left empty so the child inherits the parent's
/// environment, which MSVC and clang rely on to locate the standard library.
const std::vector<std::string>& process_env() {
    const static auto env = [] {
        std::vector<std::string> result;
        if(environ) {
            for(char** e = environ; *e; ++e) {
                if(!llvm::StringRef(*e).starts_with("LANG="))
                    result.emplace_back(*e);
            }
        }
        result.emplace_back("LANG=C");
        return result;
    }();
    return env;
}
#endif

kota::task<std::string> drain_pipe(kota::pipe p) {
    std::string buf;
    while(true) {
        auto result = co_await p.read();
        if(!result.has_value())
            break;
        auto& chunk = result.value();
        if(chunk.empty())
            break;
        buf += chunk;
    }
    co_return buf;
}

kota::task<std::expected<std::string, std::string>>
    execute_async(std::vector<std::string> arguments, bool capture_stdout = false) {
    kota::process::options opts;
    opts.file = arguments[0];
    opts.args = std::move(arguments);
#ifndef _WIN32
    opts.env = process_env();
#endif
    opts.streams = {
        kota::process::stdio::ignore(),
        kota::process::stdio::pipe(false, true),
        kota::process::stdio::pipe(false, true),
    };

    LOG_INFO("Execute command: {}", opts.file);

    auto spawn = kota::process::spawn(opts);
    if(!spawn.has_value()) {
        co_return std::unexpected(
            std::format("Failed to spawn {}: {}", opts.file, spawn.error().message()));
    }
    auto& s = *spawn;

    // Drain both pipes concurrently with process exit: a child blocking on a
    // full pipe would otherwise deadlock against our wait().
    auto [stdout_data, stderr_data] = co_await kota::when_all(drain_pipe(std::move(s.stdout_pipe)),
                                                              drain_pipe(std::move(s.stderr_pipe)));

    auto exit_result = co_await s.proc.wait();
    if(!exit_result.has_value()) {
        co_return std::unexpected(
            std::format("Process wait failed: {}", exit_result.error().message()));
    }

    auto& exit = *exit_result;
    if(exit.status != 0) {
        co_return std::unexpected(
            std::format("Process {} exited with code {}", opts.file, exit.status));
    }

    co_return capture_stdout ? std::move(stdout_data) : std::move(stderr_data);
}

std::expected<void, std::string> query_driver(
    llvm::ArrayRef<const char*> arguments,
    llvm::function_ref<void(const char* driver, llvm::ArrayRef<const char*> cc1_args)> callback) {
    /// FIXME: collect diagnostic here ...
    clang::DiagnosticOptions options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    options,
                                    new clang::IgnoringDiagConsumer());

    llvm::SmallVector<const char*, 256> list;
    list.emplace_back(arguments.consume_front());
    list.emplace_back("-fsyntax-only");
    list.append(arguments.begin(), arguments.end());
    arguments = list;

    /// Note that clang use the `ClangExecutable` to determine the driver mode when
    /// --driver-mode is not found in the arguments, and `TargetTriple` is used when
    /// non --target argument is found in the arguments list. See
    /// `clang::driver::BuildCompilation`. We use default arguments because we will
    /// inject related commands before querying.
    clang::driver::Driver driver(/*ClangExecutable=*/arguments[0],
                                 /*TargetTriple=*/llvm::sys::getDefaultTargetTriple(),
                                 /*Diags=*/engine);
    driver.setCheckInputsExist(false);
    driver.setProbePrecompiled(false);

    std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(arguments));
    if(!compilation) {
        return std::unexpected(std::format("Failed to build compilation for {}", arguments[0]));
    }

    // We expect to get back exactly one command job, if we didn't something
    // failed. Offload compilation is an exception as it creates multiple jobs. If
    // that's the case, we proceed with the first job. If caller needs a
    // particular job, it should be controlled via options (e.g.
    // --cuda-{host|device}-only for CUDA) passed to the driver.
    const clang::driver::JobList& jobs = compilation->getJobs();
    if(jobs.size() > 1) {
        for(auto& action: compilation->getActions()) {
            // On MacOSX real actions may end up being wrapped in BindArchAction
            if(llvm::isa<clang::driver::BindArchAction>(action)) {
                action = *action->input_begin();
            }
        }
    }

    auto cmd = llvm::find_if(jobs, [](const clang::driver::Command& cmd) {
        return cmd.getCreator().getName() == llvm::StringRef("clang");
    });
    if(cmd == jobs.end()) {
        return std::unexpected(std::format("No clang job found for {}", arguments[0]));
    }

    callback(arguments[0], cmd->getArguments());
    return {};
}

/// Parse the first `-cc1` line from clang `-###` output. Only the first line
/// is used: with multiple inputs the driver emits one job per input, and the
/// first corresponds to the first input file.
std::vector<std::string> parse_cc1_output(llvm::StringRef content) {
    llvm::SmallVector<llvm::StringRef> lines;
    content.split(lines, '\n', -1, false);

    for(llvm::StringRef line: lines) {
        line = line.trim();
        if(line.empty() || line.front() != '"')
            continue;

        llvm::SmallVector<const char*, 256> args;
        llvm::BumpPtrAllocator alloc;
        llvm::StringSaver saver(alloc);
        llvm::cl::TokenizeGNUCommandLine(line, saver, args);

        using namespace std::string_view_literals;
        if(args.size() < 2 || args[1] != "-cc1"sv)
            continue;

        std::vector<std::string> cc1_args;
        cc1_args.emplace_back(args[0]);
        cc1_args.emplace_back(args[1]);

        // Parse with CC1 visibility: the external driver may be newer than the
        // linked clang, so flags it emits that our cc1 does not understand
        // parse as unknown and are dropped. greedy_unknown makes an unknown
        // option consume its trailing values, so they are dropped along with
        // it instead of being misparsed as input files. Raw tokens are copied
        // through to preserve the exact spelling the driver emitted.
        // FIXME: Long-term we should unify the command pipeline so the driver
        // version always matches the embedded LLVM.
        std::vector<std::string> raw(args.begin() + 2, args.end());
        auto options =
            kota::option::ParseOptions{.greedy_unknown = true, .visibility = option::CC1Option};
        for(auto& r: option::table().parse(raw, options)) {
            if(!r.has_value() || r->id == option::OPT_UNKNOWN)
                continue;
            for(std::uint32_t i = r->index; i < r->next_index; ++i)
                cc1_args.emplace_back(raw[i]);
        }
        return cc1_args;
    }
    return {};
}

kota::task<std::expected<std::vector<std::string>, std::string>>
    query_one(llvm::ArrayRef<const char*> arguments, llvm::StringRef file) {
    if(arguments.empty())
        co_return std::unexpected(std::string("Empty arguments"));

    llvm::StringRef driver = arguments[0];

    /// Note: The name used to invoke the compiler driver affects its behavior.
    /// For example, `/usr/bin/clang++` is often a symbolic link to
    /// `/usr/lib/llvm-20/bin/clang`. Invoking it as `clang++` enables C++ mode
    /// and links C++ libraries by default, while invoking as `clang` defaults to C mode.
    /// Therefore, never use `realpath` on the initial `driver` name, as that
    /// would lose the context needed for the driver to behave correctly (and break caching).
    llvm::SmallString<128> resolved_path;
    if(!path::is_absolute(driver)) {
        /// If the path is not absolute path like g++, find it in the env vars.
        auto program = llvm::sys::findProgramByName(driver);
        if(!program)
            co_return std::unexpected(std::format("Cannot find driver: {}", driver.str()));
        resolved_path = *program;
        driver = resolved_path.c_str();
    }

    if(!fs::exists(driver) || !fs::can_execute(driver))
        co_return std::unexpected(
            std::format("Driver {} not found or not executable", driver.str()));

    llvm::SmallVector<const char*, 256> args;
    args.emplace_back(driver.data());
    args.append(arguments.begin() + 1, arguments.end());

    auto ext = path::extension(file);
    ext.consume_front(".");

    /// Create a file with same suffix of input file, because the input file may
    /// not exist in the disk.
    llvm::SmallString<64> src_path;
    if(auto e = fs::createTemporaryFile("query-toolchain", ext, src_path))
        co_return std::unexpected(std::format("Failed to create temp file: {}", e.message()));
    auto cleanup = llvm::make_scope_exit([&] {
        if(auto e = fs::remove(src_path))
            LOG_ERROR("Fail to remove temporary file: {}", e);
    });
    args.emplace_back(src_path.c_str());

    auto family = Toolchain::driver_family(driver);
    std::vector<std::string> cc1_args;

    switch(family) {
        // Query g++ or mingw toolchain info. We detect the target and corresponding
        // gcc toolchain install path as default behavior.
        case CompilerFamily::GCC: {
            std::string drv(driver.str());

            auto target = co_await execute_async({drv, "-dumpmachine"}, true);
            if(!target)
                co_return std::unexpected(std::move(target.error()));

            auto search_dirs = co_await execute_async({drv, "-print-search-dirs"}, true);
            if(!search_dirs)
                co_return std::unexpected(std::move(search_dirs.error()));

            std::string install_path;
            llvm::SmallVector<llvm::StringRef, 5> lines;
            llvm::StringRef(*search_dirs).split(lines, '\n', -1, false);
            for(auto line: lines) {
                line = line.trim();
                if(line.consume_front_insensitive("install:")) {
                    install_path = line.trim().str();
                    break;
                }
            }

            auto target_flag = "--target=" + llvm::StringRef(*target).trim().str();
            auto install_flag = "--gcc-install-dir=" + install_path;

            llvm::SmallVector<const char*, 256> gcc_args;
            gcc_args.emplace_back(driver.data());
            gcc_args.emplace_back(target_flag.c_str());
            gcc_args.emplace_back(install_flag.c_str());
            gcc_args.append(args.begin() + 1, args.end());

            auto queried =
                query_driver(gcc_args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                    cc1_args.emplace_back(d);
                    cc1_args.emplace_back("-cc1");
                    for(auto arg: cc1)
                        cc1_args.emplace_back(arg);
                });
            if(!queried)
                co_return std::unexpected(std::move(queried.error()));
            break;
        }

        // Query clang++ or any clang based toolchain, e.g. zig cc/c++. We query
        // the full cc1 command of clang toolchain as default.
        // TODO: Is armclang also compatible?
        case CompilerFamily::Clang:
        case CompilerFamily::Zig: {
            std::vector<std::string> exec_args;
            auto remaining = llvm::ArrayRef(args);

            if(family == CompilerFamily::Zig) {
                /// zig cc or zig c++ consumes two arguments.
                exec_args.emplace_back(remaining[0]);
                exec_args.emplace_back(remaining[1]);
                remaining = remaining.drop_front(2);
            } else {
                exec_args.emplace_back(remaining[0]);
                remaining = remaining.drop_front();
            }
            exec_args.emplace_back("-###");
            exec_args.emplace_back("-fsyntax-only");
            for(auto arg: remaining)
                exec_args.emplace_back(arg);

            auto content = co_await execute_async(std::move(exec_args));
            if(!content)
                co_return std::unexpected(std::move(content.error()));

            cc1_args = parse_cc1_output(*content);
            break;
        }

        case CompilerFamily::MSVC:
        case CompilerFamily::ClangCL: {
            llvm::SmallVector<const char*, 256> msvc_args;
            msvc_args.emplace_back(args[0]);
            /// When clang in cl mode, the target will be set to windows-msvc automatically.
            /// We don't need to add extra flag.
            msvc_args.emplace_back("--driver-mode=cl");
            msvc_args.append(args.begin() + 1, args.end());

            // No "-cc1" is inserted here: --driver-mode=cl only selects the
            // driver mode, the clang driver itself handles the rest.
            auto queried =
                query_driver(msvc_args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                    cc1_args.emplace_back(d);
                    for(auto arg: cc1)
                        cc1_args.emplace_back(arg);
                });
            if(!queried)
                co_return std::unexpected(std::move(queried.error()));
            break;
        }

        default: {
            /// TODO: nvcc and intel compilers need further exploration.
            LOG_ERROR("Unsupported compiler family: {}, driver is {}",
                      kota::meta::enum_name(family),
                      driver);

            auto queried = query_driver(args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                cc1_args.emplace_back(d);
                cc1_args.emplace_back("-cc1");
                for(auto arg: cc1)
                    cc1_args.emplace_back(arg);
            });
            if(!queried)
                co_return std::unexpected(std::move(queried.error()));
            break;
        }
    }

    // Strip the temporary probe file so results contain no input path
    // (to_argv() appends the real source file at the end). Also strip module
    // output flags the driver derives from the probe input (clang >= 22 emits
    // -fmodules-reduced-bmi -fmodule-output=<probe>.pcm for module units);
    // they reference the deleted temp file and clice manages outputs itself.
    // The probe path uses an exact match: all supported drivers echo input
    // paths verbatim, without canonicalizing them.
    std::erase_if(cc1_args, [&](const std::string& arg) {
        llvm::StringRef s(arg);
        return s == src_path || s == "-fmodules-reduced-bmi" || s.starts_with("-fmodule-output");
    });

    if(cc1_args.empty())
        co_return std::unexpected(std::format("No cc1 args produced for {}", file.str()));

    co_return cc1_args;
}

struct PendingQuery {
    std::string key;
    std::vector<const char*> query_args;
    /// Points to interned, pointer-stable storage in CompileCommand::source_file;
    /// valid for the whole warm() call.
    llvm::StringRef file;
};

}  // namespace

Toolchain::Toolchain() :
    allocator(std::make_unique<llvm::BumpPtrAllocator>()), strings(allocator.get()) {}

Toolchain::~Toolchain() = default;

CompilerFamily Toolchain::driver_family(llvm::StringRef driver) {
    auto try_get = [](llvm::StringRef name) {
        if(name == "cl")
            return CompilerFamily::MSVC;
        if(name == "nvcc")
            return CompilerFamily::NVCC;
        if(name.ends_with("clang-cl"))
            return CompilerFamily::ClangCL;
        if(name.ends_with("clang") || name.ends_with("clang++"))
            return CompilerFamily::Clang;
        // Intel must precede GCC: `icc` would otherwise match ends_with("cc").
        if(name.contains("icpc") || name.contains("icc") || name.contains("dpcpp") ||
           name.contains("icx"))
            return CompilerFamily::Intel;
        if(name.ends_with("cc") || name.ends_with("c++") || name.ends_with("gcc") ||
           name.ends_with("g++"))
            return CompilerFamily::GCC;
        if(name.ends_with("zig"))
            return CompilerFamily::Zig;
        return CompilerFamily::Unknown;
    };

    auto name = llvm::sys::path::filename(driver);
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping the executable suffix: clang++.exe -> clang++
    name.consume_back(".exe");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping any trailing version number: clang++3.5 -> clang++
    name = name.rtrim("0123456789.-");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    // Stripping trailing -component: clang++-tot -> clang++
    name = name.slice(0, name.rfind('-'));
    return try_get(name);
}

std::expected<std::vector<std::string>, std::string>
    Toolchain::query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file) {
    std::expected<std::vector<std::string>, std::string> result;
    kota::event_loop loop;
    auto task = [&]() -> kota::task<> {
        result = co_await query_one(arguments, file);
    };
    loop.schedule(task());
    loop.run();
    return result;
}

Toolchain::ToolchainExtract Toolchain::extract_flags(llvm::StringRef file,
                                                     llvm::ArrayRef<const char*> arguments) {
    ToolchainExtract result;

    result.key += arguments[0];
    result.key += '\0';

    result.key += path::extension(file);
    result.key += '\0';

    result.query_args.push_back(arguments[0]);

    std::vector<std::string> parse_args(arguments.begin() + 1, arguments.end());
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                              .visibility = default_visibility(arguments[0])};
    for(auto& r: option::table().parse(parse_args, options)) {
        if(!r.has_value())
            continue;
        auto& arg = *r;

        // User-content options (-I, -D, ...) don't affect the toolchain query;
        // resolve() re-appends them from the original command. Everything else
        // may change driver behavior, so it goes into both key and query.
        if(is_user_content_option(arg.id))
            continue;

        result.key += std::to_string(arg.id);
        result.key += '\0';
        for(auto value: arg.values) {
            result.key += value;
            result.key += '\0';
        }

        auto cb = [&](std::string_view s) {
            result.query_args.push_back(strings.save(s).data());
        };
        option::table().render(arg, cb);
    }

    return result;
}

std::expected<void, std::string> Toolchain::resolve(CompileCommand& cmd) {
    if(cmd.resolved.flags.empty())
        return std::unexpected("empty flags");

    auto [key, query_args] = extract_flags(cmd.source_file, cmd.resolved.flags);

    auto it = cache.find(key);
    if(it == cache.end()) {
        if(auto failed_it = failed.find(key); failed_it != failed.end())
            return std::unexpected(failed_it->second);

        LOG_WARN("Toolchain cache miss: file={}", cmd.source_file);

        auto result = query(query_args, cmd.source_file);
        if(!result) {
            failed.try_emplace(key, result.error());
            return std::unexpected(std::move(result.error()));
        }

        std::vector<const char*> saved;
        saved.reserve(result->size());
        for(auto& s: *result)
            saved.push_back(strings.save(s).data());
        it = cache.try_emplace(std::move(key), std::move(saved)).first;
    }

    auto cached = llvm::ArrayRef(it->second);
    std::vector<const char*> new_flags(cached.begin(), cached.end());

    // Replace resource dir in cc1 result with ours.
    if(!resource_dir().empty()) {
        llvm::StringRef old_resource_dir;
        for(std::size_t i = 0; i + 1 < new_flags.size(); ++i) {
            if(new_flags[i] == llvm::StringRef("-resource-dir")) {
                old_resource_dir = new_flags[i + 1];
                break;
            }
        }
        if(!old_resource_dir.empty() && old_resource_dir != resource_dir()) {
            for(auto& arg: new_flags) {
                llvm::StringRef s(arg);
                if(s.starts_with(old_resource_dir)) {
                    auto replaced = resource_dir().str() + s.substr(old_resource_dir.size()).str();
                    arg = strings.save(replaced).data();
                }
            }
        }
    }

    // Extract user-content flags from original command and append to cc1 result.
    std::vector<std::string> resolve_parse_args(cmd.resolved.flags.begin() + 1,
                                                cmd.resolved.flags.end());
    auto resolve_options =
        kota::option::ParseOptions{.dash_dash_parsing = true,
                                   .visibility = default_visibility(cmd.resolved.flags[0])};
    for(auto& r: option::table().parse(resolve_parse_args, resolve_options)) {
        if(!r.has_value())
            continue;
        auto& arg = *r;
        if(is_user_content_option(arg.id)) {
            auto cb = [&](std::string_view s) {
                new_flags.push_back(strings.save(s).data());
            };
            option::table().render(arg, cb);
        }
    }

    // Strip -main-file-name and its value (to_argv() will re-inject with correct basename).
    std::vector<const char*> cleaned;
    cleaned.reserve(new_flags.size());
    for(std::size_t i = 0; i < new_flags.size(); ++i) {
        if(new_flags[i] == llvm::StringRef("-main-file-name") && i + 1 < new_flags.size()) {
            ++i;
            continue;
        }
        cleaned.push_back(new_flags[i]);
    }

    cmd.resolved.flags = std::move(cleaned);
    cmd.resolved.is_cc1 = ranges::contains(cmd.resolved.flags, llvm::StringRef("-cc1"));
    return {};
}

void Toolchain::resolve_or_warn(CompileCommand& cmd) {
    if(auto result = resolve(cmd); !result) {
        LOG_WARN("Toolchain resolve failed for {}: {}", cmd.source_file, result.error());
    }
}

bool Toolchain::has_cache() const {
    return !cache.empty();
}

void Toolchain::warm(llvm::ArrayRef<CompileCommand> commands) {
    llvm::StringMap<bool> seen;
    std::vector<PendingQuery> pending;

    for(auto& cmd: commands) {
        if(cmd.resolved.flags.empty())
            continue;

        auto [key, query_args] = extract_flags(cmd.source_file, cmd.resolved.flags);
        if(cache.count(key) || failed.count(key) || !seen.try_emplace(key, true).second)
            continue;

        pending.push_back({std::move(key), std::move(query_args), cmd.source_file});
    }

    if(pending.empty())
        return;

    auto total = pending.size();
    LOG_INFO("Warming toolchain cache: {} unique queries", total);

    // Run the queries concurrently on a local event loop. The interface
    // stays synchronous: block until all complete, then fill the cache here.
    struct QueryOutcome {
        std::string key;
        std::expected<std::vector<std::string>, std::string> result;
    };

    // The query is moved into the coroutine frame as a parameter, so the
    // argument references stay valid for the coroutine's whole lifetime.
    auto make_task = [](PendingQuery q) -> kota::task<QueryOutcome> {
        auto result = co_await query_one(q.query_args, q.file);
        co_return QueryOutcome{std::move(q.key), std::move(result)};
    };

    kota::small_vector<QueryOutcome> outcomes;

    kota::event_loop loop;
    auto run = [&]() -> kota::task<> {
        std::vector<kota::task<QueryOutcome>> tasks;
        tasks.reserve(pending.size());
        for(auto& q: pending)
            tasks.push_back(make_task(std::move(q)));

        outcomes = co_await kota::when_all(std::move(tasks));
    };
    loop.schedule(run());
    loop.run();

    std::size_t succeeded = 0;
    for(auto& o: outcomes) {
        if(!o.result) {
            LOG_ERROR("Toolchain query failed: {}", o.result.error());
            failed.try_emplace(std::move(o.key), std::move(o.result.error()));
            continue;
        }

        std::vector<const char*> saved;
        saved.reserve(o.result->size());
        for(auto& arg: *o.result)
            saved.push_back(strings.save(arg).data());
        cache.try_emplace(std::move(o.key), std::move(saved));
        succeeded += 1;
    }

    LOG_INFO("Toolchain cache warmed: {} succeeded, {} failed", succeeded, total - succeeded);
}

#ifdef CLICE_ENABLE_TEST

std::vector<std::string> Toolchain::parse_cc1(llvm::StringRef content) {
    return parse_cc1_output(content);
}

#endif

}  // namespace clice
