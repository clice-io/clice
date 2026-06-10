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
llvm::ArrayRef<llvm::StringRef> process_env() {
    static std::vector<std::string> storage;
    const static auto refs = [] {
        if(environ) {
            for(char** e = environ; *e; ++e) {
                if(!llvm::StringRef(*e).starts_with("LANG="))
                    storage.emplace_back(*e);
            }
        }
        storage.emplace_back("LANG=C");
        return std::vector<llvm::StringRef>(storage.begin(), storage.end());
    }();
    return refs;
}
#endif

#ifdef _WIN32
constexpr llvm::StringRef null_dev = "NUL";
#else
constexpr llvm::StringRef null_dev = "/dev/null";
#endif

std::expected<std::string, std::string> execute(llvm::ArrayRef<std::string> arguments,
                                                bool capture_stdout = false) {
    LOG_INFO("Execute command: {}", arguments[0]);

    llvm::SmallString<64> out_path;
    if(auto e = fs::createTemporaryFile("query-toolchain", "out", out_path)) {
        return std::unexpected(std::format("Failed to create temp file: {}", e.message()));
    }
    auto cleanup = llvm::make_scope_exit([&] {
        if(auto e = fs::remove(out_path))
            LOG_ERROR("Fail to remove temporary file: {}", e);
    });

#ifdef _WIN32
    /// Inherit env from the parent process: MSVC and clang on Windows rely on
    /// environment variables to locate the standard library.
    constexpr auto env = std::nullopt;
#else
    /// On POSIX, inherit env but pin LANG=C so driver output is not localized.
    auto env = process_env();
#endif

    std::optional<llvm::StringRef> redirects[3] = {
        {null_dev},                                    // stdin
        {capture_stdout ? out_path.str() : null_dev},  // stdout
        {capture_stdout ? null_dev : out_path.str()},  // stderr
    };

    llvm::SmallVector<llvm::StringRef> argv(arguments.begin(), arguments.end());

    std::string message;
    if(int rc = llvm::sys::ExecuteAndWait(argv[0],
                                          argv,
                                          env,
                                          redirects,
                                          /*SecondsToWait=*/0,
                                          /*MemoryLimit=*/0,
                                          &message)) {
        return std::unexpected(
            std::format("Process {} exited with code {}: {}", argv[0].str(), rc, message));
    }

    auto file = llvm::MemoryBuffer::getFile(out_path);
    if(!file) {
        return std::unexpected(std::format("Failed to read output of {}: {}",
                                           argv[0].str(),
                                           file.getError().message()));
    }

    return std::string((*file)->getBuffer());
}

std::expected<void, std::string> query_driver(
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
        return std::unexpected(std::format("Failed to build compilation for {}", arguments[0]));
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

std::expected<std::vector<std::string>, std::string>
    query_one(llvm::ArrayRef<const char*> arguments, llvm::StringRef file) {
    if(arguments.empty())
        return std::unexpected(std::string("Empty arguments"));

    llvm::StringRef driver = arguments[0];

    llvm::SmallString<128> resolved_path;
    if(!path::is_absolute(driver)) {
        auto program = llvm::sys::findProgramByName(driver);
        if(!program)
            return std::unexpected(std::format("Cannot find driver: {}", driver.str()));
        resolved_path = *program;
        driver = resolved_path.c_str();
    }

    if(!fs::exists(driver) || !fs::can_execute(driver))
        return std::unexpected(std::format("Driver {} not found or not executable", driver.str()));

    llvm::SmallVector<const char*, 256> args;
    args.emplace_back(driver.data());
    args.append(arguments.begin() + 1, arguments.end());

    auto ext = path::extension(file);
    ext.consume_front(".");

    llvm::SmallString<64> src_path;
    if(auto e = fs::createTemporaryFile("query-toolchain", ext, src_path))
        return std::unexpected(std::format("Failed to create temp file: {}", e.message()));
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

            auto target = execute({drv, "-dumpmachine"}, true);
            if(!target)
                return std::unexpected(std::move(target.error()));

            auto search_dirs = execute({drv, "-print-search-dirs"}, true);
            if(!search_dirs)
                return std::unexpected(std::move(search_dirs.error()));

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
                return std::unexpected(std::move(queried.error()));
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

            auto content = execute(std::move(exec_args));
            if(!content)
                return std::unexpected(std::move(content.error()));

            cc1_args = parse_cc1_output(*content);
            break;
        }

        case CompilerFamily::MSVC:
        case CompilerFamily::ClangCL: {
            llvm::SmallVector<const char*, 256> msvc_args;
            msvc_args.emplace_back(args[0]);
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
                return std::unexpected(std::move(queried.error()));
            break;
        }

        default: {
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
                return std::unexpected(std::move(queried.error()));
            break;
        }
    }

    // Strip the temporary probe file so results contain no input path
    // (to_argv() appends the real source file at the end). Also strip module
    // output flags the driver derives from the probe input (clang >= 22 emits
    // -fmodules-reduced-bmi -fmodule-output=<probe>.pcm for module units);
    // they reference the deleted temp file and clice manages outputs itself.
    std::erase_if(cc1_args, [&](const std::string& arg) {
        llvm::StringRef s(arg);
        return s == src_path || s == "-fmodules-reduced-bmi" || s.starts_with("-fmodule-output");
    });

    if(cc1_args.empty())
        return std::unexpected(std::format("No cc1 args produced for {}", file.str()));

    return cc1_args;
}

struct PendingQuery {
    std::string key;
    std::vector<const char*> query_args;
    std::string file;
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

std::expected<std::vector<std::string>, std::string>
    Toolchain::query(llvm::ArrayRef<const char*> arguments, llvm::StringRef file) {
    return query_one(arguments, file);
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
        LOG_WARN("Toolchain cache miss: file={}", cmd.source_file);

        auto result = query(query_args, cmd.source_file);
        if(!result)
            return std::unexpected(std::move(result.error()));

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
        if(cache.count(key) || !seen.try_emplace(key, true).second)
            continue;

        pending.push_back({std::move(key), std::move(query_args), std::string(cmd.source_file)});
    }

    if(pending.empty())
        return;

    auto total = pending.size();
    LOG_INFO("Warming toolchain cache: {} unique queries", total);

    std::size_t succeeded = 0;
    for(auto& q: pending) {
        auto result = query_one(q.query_args, q.file);
        if(!result) {
            LOG_ERROR("Toolchain query failed: {}", result.error());
            continue;
        }

        std::vector<const char*> saved;
        saved.reserve(result->size());
        for(auto& arg: *result)
            saved.push_back(strings.save(arg).data());
        cache.try_emplace(std::move(q.key), std::move(saved));
        succeeded += 1;
    }

    LOG_INFO("Toolchain cache warmed: {} succeeded, {} failed", succeeded, total - succeeded);
}

}  // namespace clice
