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
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Host.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/Tool.h"

#ifndef _WIN32
#include <unistd.h>
extern char** environ;

const static std::vector<std::string>& process_env() {
    static auto env = [] {
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

#ifdef _WIN32
llvm::StringRef null_dev = "NUL";
#else
llvm::StringRef null_dev = "/dev/null";
#endif

namespace clice {

namespace {

namespace ranges = std::ranges;

// ── Async subprocess helpers ───────────────────────────────────────────

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

    LOG_INFO("Execute async: {}", opts.file);

    auto spawn = kota::process::spawn(opts);
    if(!spawn.has_value()) {
        co_return std::unexpected(
            std::format("Failed to spawn {}: {}", opts.file, spawn.error().message()));
    }
    auto& s = *spawn;

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

// ── In-process clang Driver API ────────────────────────────────────────

bool query_driver(
    llvm::ArrayRef<const char*> arguments,
    llvm::function_ref<void(const char* driver, llvm::ArrayRef<const char*> cc1_args)> callback) {
    clang::DiagnosticOptions options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    options,
                                    new clang::IgnoringDiagConsumer());

    llvm::SmallVector<const char*, 256> list;
    list.emplace_back(arguments.consume_front());
    list.emplace_back("-fsyntax-only");
    list.append(arguments.begin(), arguments.end());
    arguments = list;

    clang::driver::Driver driver(/*ClangExecutable=*/arguments[0],
                                 /*TargetTriple=*/llvm::sys::getDefaultTargetTriple(),
                                 /*Diags=*/engine);
    driver.setCheckInputsExist(false);
    driver.setProbePrecompiled(false);

    std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(arguments));
    if(!compilation) {
        LOG_ERROR_RET(false, "Fail to query driver");
    }

    const clang::driver::JobList& jobs = compilation->getJobs();
    if(jobs.size() > 1) {
        for(auto& action: compilation->getActions()) {
            if(llvm::isa<clang::driver::BindArchAction>(action)) {
                action = *action->input_begin();
            }
        }
    }

    auto cmd = llvm::find_if(jobs, [](const clang::driver::Command& cmd) {
        return cmd.getCreator().getName() == llvm::StringRef("clang");
    });
    if(cmd == jobs.end()) {
        LOG_ERROR_RET(false, "Fail to query driver, clang job was not found!");
    }

    callback(arguments[0], cmd->getArguments());
    return true;
}

// ── Parse clang -### output ────────────────────────────────────────────

std::vector<std::string> parse_cc1_output(llvm::StringRef content) {
    std::vector<std::string> cc1_args;
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

        auto& table = clang::driver::getDriverOptTable();
        auto raw = llvm::ArrayRef(args).drop_front(2);
        unsigned mi = 0, mc = 0;
        auto parsed = table.ParseArgs(raw, mi, mc);

        llvm::DenseSet<unsigned> unknown;
        for(auto* a: parsed)
            if(a->getOption().getKind() == llvm::opt::Option::UnknownClass)
                unknown.insert(a->getIndex());

        cc1_args.emplace_back(args[0]);
        cc1_args.emplace_back(args[1]);
        for(unsigned i = 0; i < raw.size(); ++i) {
            if(unknown.contains(i) || raw[i] == "-###"sv)
                continue;
            cc1_args.emplace_back(raw[i]);
        }
    }
    return cc1_args;
}

// ── Core async query (single implementation for all paths) ─────────────

kota::task<std::expected<std::vector<std::string>, std::string>>
    query_one(llvm::ArrayRef<const char*> arguments, llvm::StringRef file) {
    if(arguments.empty())
        co_return std::unexpected(std::string("Empty arguments"));

    llvm::StringRef driver = arguments[0];

    llvm::SmallString<128> resolved_path;
    if(!path::is_absolute(driver)) {
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
        case CompilerFamily::GCC: {
            std::string drv(driver.str());

            std::string target;
            if(auto r = co_await execute_async({drv, "-dumpmachine"}, true))
                target = llvm::StringRef(*r).trim().str();

            std::string install_path;
            if(auto r = co_await execute_async({drv, "-print-search-dirs"}, true)) {
                llvm::SmallVector<llvm::StringRef, 5> lines;
                llvm::StringRef(*r).split(lines, '\n', -1, false);
                for(auto line: lines) {
                    line = line.trim();
                    if(line.consume_front_insensitive("install:")) {
                        install_path = line.trim().str();
                        break;
                    }
                }
            }

            auto target_flag = "--target=" + target;
            auto install_flag = "--gcc-install-dir=" + install_path;

            llvm::SmallVector<const char*, 256> gcc_args;
            gcc_args.emplace_back(driver.data());
            gcc_args.emplace_back(target_flag.c_str());
            gcc_args.emplace_back(install_flag.c_str());
            gcc_args.append(args.begin() + 1, args.end());

            query_driver(gcc_args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                cc1_args.emplace_back(d);
                cc1_args.emplace_back("-cc1");
                for(auto arg: cc1)
                    cc1_args.emplace_back(arg);
            });
            break;
        }

        case CompilerFamily::Clang:
        case CompilerFamily::Zig: {
            std::vector<std::string> exec_args;
            auto remaining = llvm::ArrayRef(args);

            if(family == CompilerFamily::Zig) {
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
            msvc_args.emplace_back("--driver-mode=cl");
            msvc_args.append(args.begin() + 1, args.end());

            query_driver(msvc_args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                cc1_args.emplace_back(d);
                for(auto arg: cc1)
                    cc1_args.emplace_back(arg);
            });
            break;
        }

        default: {
            LOG_ERROR("Unsupported compiler family: {}, driver is {}",
                      kota::meta::enum_name(family),
                      driver);

            query_driver(args, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                cc1_args.emplace_back(d);
                cc1_args.emplace_back("-cc1");
                for(auto arg: cc1)
                    cc1_args.emplace_back(arg);
            });
            break;
        }
    }

    if(cc1_args.empty())
        co_return std::unexpected(std::format("No cc1 args produced for {}", file.str()));

    co_return cc1_args;
}

// ── Batch parallel wrapper ─────────────────────────────────────────────

struct PendingQuery {
    std::string key;
    std::vector<const char*> query_args;
    std::string file;
};

struct QueryOutcome {
    std::string key;
    std::vector<std::string> cc1_args;
};

kota::task<std::vector<QueryOutcome>> run_queries(std::vector<PendingQuery> pending) {
    auto make_task = [](PendingQuery q) -> kota::task<std::expected<QueryOutcome, std::string>> {
        auto result = co_await query_one(q.query_args, q.file);
        if(!result)
            co_return std::unexpected(std::move(result.error()));
        co_return QueryOutcome{std::move(q.key), std::move(*result)};
    };

    std::vector<kota::task<std::expected<QueryOutcome, std::string>>> tasks;
    tasks.reserve(pending.size());
    for(auto& q: pending)
        tasks.push_back(make_task(std::move(q)));

    auto outcomes = co_await kota::when_all(std::move(tasks));

    std::vector<QueryOutcome> successful;
    for(auto& r: outcomes) {
        if(r.has_value()) {
            successful.push_back(std::move(*r));
        } else {
            LOG_ERROR("Toolchain query failed: {}", r.error());
        }
    }

    co_return successful;
}

}  // namespace

// ── Toolchain implementation ───────────────────────────────────────────

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
        if(name.ends_with("cc") || name.ends_with("c++") || name.ends_with("gcc") ||
           name.ends_with("g++"))
            return CompilerFamily::GCC;
        if(name.contains("icpc") || name.contains("icc") || name.contains("dpcpp") ||
           name.contains("icx"))
            return CompilerFamily::Intel;
        if(name.ends_with("zig"))
            return CompilerFamily::Zig;
        return CompilerFamily::Unknown;
    };

    auto name = llvm::sys::path::filename(driver);
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    name.consume_back(".exe");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    name = name.rtrim("0123456789.-");
    if(auto f = try_get(name); f != CompilerFamily::Unknown)
        return f;

    name = name.slice(0, name.rfind('-'));
    return try_get(name);
}

std::vector<std::string> Toolchain::query(llvm::ArrayRef<const char*> arguments,
                                          llvm::StringRef file) {
    std::expected<std::vector<std::string>, std::string> result;
    kota::event_loop loop;
    loop.schedule([&]() -> kota::task<> {
        result = co_await query_one(arguments, file);
    }());
    loop.run();

    if(!result) {
        LOG_ERROR("Toolchain query failed: {}", result.error());
        return {};
    }
    return std::move(*result);
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
        if(!is_toolchain_option(arg.id))
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

bool Toolchain::resolve(CompileCommand& cmd) {
    if(cmd.resolved.flags.empty())
        return false;

    auto [key, query_args] = extract_flags(cmd.source_file, cmd.resolved.flags);

    auto it = cache.find(key);
    if(it == cache.end()) {
        LOG_WARN("Toolchain cache miss: file={}", cmd.source_file);

        auto result = query(query_args, cmd.source_file);
        if(result.empty())
            return false;

        std::vector<const char*> saved;
        saved.reserve(result.size());
        for(auto& s: result)
            saved.push_back(strings.save(s).data());
        it = cache.try_emplace(std::move(key), std::move(saved)).first;
    }

    auto cached = llvm::ArrayRef(it->second);
    if(cached.empty())
        return false;

    std::vector<const char*> new_flags(cached.begin(), cached.end());
    new_flags.pop_back();  // remove temp source file

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
    return true;
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
        if(cache.count(key) || !seen.try_emplace(key, true).second)
            continue;

        pending.push_back({std::move(key), std::move(query_args), std::string(cmd.source_file)});
    }

    if(pending.empty())
        return;

    auto total = pending.size();
    LOG_INFO("Warming toolchain cache: {} unique queries", total);

    std::vector<QueryOutcome> results;
    kota::event_loop loop;
    loop.schedule([&]() -> kota::task<> {
        results = co_await run_queries(std::move(pending));
    }());
    loop.run();

    for(auto& r: results) {
        if(cache.count(r.key))
            continue;
        std::vector<const char*> saved;
        saved.reserve(r.cc1_args.size());
        for(auto& arg: r.cc1_args)
            saved.push_back(strings.save(arg).data());
        cache.try_emplace(std::move(r.key), std::move(saved));
    }

    LOG_INFO("Toolchain cache warmed: {} succeeded, {} failed",
             results.size(),
             total - results.size());
}

}  // namespace clice
