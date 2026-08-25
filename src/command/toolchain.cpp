#include "command/toolchain.h"

#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include "command/argument_parser.h"
#include "command/command.h"
#include "command/nvcc.h"
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
#include "llvm/TargetParser/Triple.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"

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
            // A newer external driver may emit known options with values our
            // linked clang cannot parse (e.g. -mframe-pointer=non-leaf-no-reserve).
            // Codegen options are irrelevant for syntax analysis; stripping them
            // avoids CompilerInvocation::CreateFromArgs failures on new values.
            if(is_codegen_option(r->id))
                continue;
            for(std::uint32_t i = r->index; i < r->next_index; ++i)
                cc1_args.emplace_back(raw[i]);
        }
        return cc1_args;
    }
    return {};
}

/// The two flags that pin a gcc-style toolchain for our driver: the target
/// triple and the gcc installation clang should derive its paths from.
struct GCCToolchainFlags {
    std::string target;
    std::string install_dir;
};

kota::task<std::expected<GCCToolchainFlags, std::string>> query_gcc_flags(std::string driver) {
    auto target = co_await execute_async({driver, "-dumpmachine"}, true);
    if(!target)
        co_return std::unexpected(std::move(target.error()));

    auto search_dirs = co_await execute_async({driver, "-print-search-dirs"}, true);
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

    co_return GCCToolchainFlags{
        .target = "--target=" + llvm::StringRef(*target).trim().str(),
        .install_dir = "--gcc-install-dir=" + install_path,
    };
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

    /// .cuh is not a clang-known extension: without a language override the
    /// probe counts as linker input and the driver builds no compile job.
    if(ext == "cuh" && !ranges::any_of(args, [](llvm::StringRef arg) { return arg == "-x"; }))
        args.append({"-x", "cuda"});

    args.emplace_back(src_path.c_str());

    auto family = Toolchain::driver_family(driver);
    std::vector<std::string> cc1_args;

    switch(family) {
        // Query g++ or mingw toolchain info. We detect the target and corresponding
        // gcc toolchain install path as default behavior.
        case CompilerFamily::GCC: {
            auto gcc = co_await query_gcc_flags(driver.str());
            if(!gcc)
                co_return std::unexpected(std::move(gcc.error()));

            llvm::SmallVector<const char*, 256> gcc_args;
            gcc_args.emplace_back(driver.data());
            gcc_args.emplace_back(gcc->target.c_str());
            gcc_args.emplace_back(gcc->install_dir.c_str());
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

        // Query nvcc by asking for its compilation pipeline: the dryrun
        // names the toolkit root, the host compiler and the macros nvcc
        // injects — none of which clang can derive on its own. The host
        // toolchain is then pinned as in the GCC branch and our own driver
        // builds the cc1 in CUDA mode.
        case CompilerFamily::NVCC: {
            /// nvcc only offloads CUDA-language inputs; a host-language
            /// entry (`nvcc -c foo.cpp`, or an explicit `-x c++`) compiles
            /// in a single host step. The dryrun probe follows the effective
            /// language so it reports the pipeline the real file gets —
            /// only a .cu input makes nvcc print the offload pipeline (the
            /// shared probe keeps the real file's extension).
            bool cuda_input = ext == "cu" || ext == "cuh";
            for(std::size_t i = 0; i + 1 < args.size(); i += 1) {
                if(llvm::StringRef(args[i]) == "-x")
                    cuda_input = llvm::StringRef(args[i + 1]) == "cuda";
            }
            llvm::StringRef probe_ext = "cu";
            if(!cuda_input)
                probe_ext = (ext == "cu" || ext == "cuh") ? "cpp" : ext;

            llvm::SmallString<64> nvcc_probe;
            if(auto e = fs::createTemporaryFile("query-toolchain", probe_ext, nvcc_probe))
                co_return std::unexpected(
                    std::format("Failed to create temp file: {}", e.message()));
            auto nvcc_cleanup = llvm::make_scope_exit([&] {
                if(auto e = fs::remove(nvcc_probe))
                    LOG_ERROR("Fail to remove temporary file: {}", e);
            });

            std::vector<std::string> dryrun_args = {driver.str(),
                                                    "--dryrun",
                                                    "-c",
                                                    std::string(nvcc_probe)};
            for(llvm::StringRef arg: args) {
                if(is_nvcc_probe_flag(arg))
                    dryrun_args.push_back(arg.str());
            }

            auto dryrun = co_await execute_async(std::move(dryrun_args));
            if(!dryrun)
                co_return std::unexpected(std::move(dryrun.error()));

            auto info = parse_nvcc_dryrun(*dryrun);
            if(!info)
                co_return std::unexpected(std::move(info.error()));

            /// The dryrun spells the host compiler the way nvcc resolves it —
            /// often a bare name that only exists on nvcc's own augmented
            /// PATH. Try that PATH, then next to the nvcc binary, then ours.
            std::string host = info->host_compiler;
            if(!path::is_absolute(host)) {
                std::string resolved_host;
                for(auto& dir: info->search_path) {
                    auto candidate = path::join(dir, host);
                    if(fs::exists(candidate) && fs::can_execute(candidate)) {
                        resolved_host = std::move(candidate);
                        break;
                    }
                }
                if(resolved_host.empty()) {
                    auto sibling = path::join(path::parent_path(driver), host);
                    if(fs::exists(sibling) && fs::can_execute(sibling))
                        resolved_host = std::move(sibling);
                }
                if(resolved_host.empty()) {
                    if(auto program = llvm::sys::findProgramByName(host))
                        resolved_host = std::move(*program);
                }
                if(resolved_host.empty())
                    co_return std::unexpected(
                        std::format("Cannot find nvcc host compiler: {}", host));
                host = std::move(resolved_host);
            }

            std::vector<std::string> cuda_args;
            cuda_args.push_back(host);

            switch(Toolchain::driver_family(host)) {
                case CompilerFamily::GCC: {
                    auto gcc = co_await query_gcc_flags(host);
                    if(!gcc)
                        co_return std::unexpected(std::move(gcc.error()));
                    cuda_args.push_back(std::move(gcc->target));
                    cuda_args.push_back(std::move(gcc->install_dir));
                    break;
                }
                /// A clang host needs no pin: the in-process driver already
                /// defaults to the running machine's triple.
                case CompilerFamily::Clang: break;
                default:
                    co_return std::unexpected(
                        std::format("Unsupported nvcc host compiler: {}", host));
            }

            if(cuda_input) {
                /// Without a root from the dryrun, clang's own CUDA detection
                /// (which knows the distro-specific install locations) is the
                /// remaining chance.
                if(!info->cuda_path.empty())
                    cuda_args.push_back("--cuda-path=" + info->cuda_path);
                /// A toolkit newer than the linked clang still parses, modulo
                /// missing feature macros; without this the version warning
                /// would land in every file's diagnostics.
                cuda_args.push_back("--no-cuda-version-check");

                auto contains_prefix = [&](std::initializer_list<llvm::StringRef> prefixes) {
                    return ranges::any_of(args, [&](llvm::StringRef arg) {
                        return ranges::any_of(prefixes, [&](llvm::StringRef prefix) {
                            return arg.starts_with(prefix);
                        });
                    });
                };

                /// Default to the device-side view: reading CUDA code means
                /// reading the `__CUDA_ARCH__` world — CUTLASS keeps every
                /// arch-specific path behind it, and the host pass grays them
                /// all out. `--cuda-host-only` in the command (e.g. a config
                /// rule append) flips a file to the host view instead; the
                /// last selector wins, matching the driver's own reading of
                /// the forwarded flags.
                std::optional<bool> selected_host;
                for(llvm::StringRef arg: args) {
                    if(arg == "--cuda-host-only")
                        selected_host = true;
                    else if(arg == "--cuda-device-only")
                        selected_host = false;
                }
                bool host_view = selected_host.value_or(false);
                if(!selected_host.has_value())
                    cuda_args.push_back("--cuda-device-only");

                for(auto& define: host_view ? info->host_defines : info->device_defines)
                    cuda_args.push_back("-D" + define);

                if(!contains_prefix({"-std="}) && !info->cpp_dialect.empty())
                    cuda_args.push_back("-std=" + info->cpp_dialect);

                /// An `-arch=` probe token is an explicit selection too: its
                /// resolution replaces it in place below.
                if(!contains_prefix({"--cuda-gpu-arch=", "--offload-arch=", "-arch="}) &&
                   !info->default_arch.empty())
                    cuda_args.push_back("--cuda-gpu-arch=" + info->default_arch);
            } else {
                /// The single host step still carries nvcc's identity
                /// macros (__NVCC__, version macros) — but no CUDA mode.
                for(auto& define: info->host_defines)
                    cuda_args.push_back("-D" + define);
            }

            for(llvm::StringRef arg: llvm::ArrayRef(args).drop_front()) {
                if(is_nvcc_probe_flag(arg)) {
                    /// The dryrun's resolution of an `-arch=<special>` token
                    /// lands exactly where the token sat: an edit-appended
                    /// selection carries `--no-offload-arch=all` right
                    /// before it, and inserting the resolution any earlier
                    /// would put it on the cleared side.
                    if(cuda_input && arg.starts_with("-arch=") && !info->default_arch.empty())
                        cuda_args.push_back("--cuda-gpu-arch=" + info->default_arch);
                    continue;
                }
                cuda_args.push_back(arg.str());
            }

            std::vector<const char*> cuda_argv;
            cuda_argv.reserve(cuda_args.size());
            for(auto& arg: cuda_args)
                cuda_argv.push_back(arg.c_str());

            auto queried =
                query_driver(cuda_argv, [&](const char* d, llvm::ArrayRef<const char*> cc1) {
                    cc1_args.emplace_back(d);
                    cc1_args.emplace_back("-cc1");
                    for(auto arg: cc1)
                        cc1_args.emplace_back(arg);
                });
            if(!queried)
                co_return std::unexpected(std::move(queried.error()));
            break;
        }

        default: {
            /// TODO: intel compilers need further exploration.
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

/// Whether the command targets windows-gnu: an explicit target flag wins
/// (the last one, matching the driver's getLastArg); otherwise a
/// target-prefixed driver name (llvm-mingw installs
/// `x86_64-w64-mingw32-clang++`-style wrappers that derive the target
/// implicitly) decides. Parsing resolves aliases, so the legacy `-target`
/// and `=` spellings arrive as the canonical ids.
static bool uses_windows_gnu_target(llvm::ArrayRef<const char*> arguments) {
    std::vector<std::string> parse_args(arguments.begin() + 1, arguments.end());
    auto options = kota::option::ParseOptions{.dash_dash_parsing = true,
                                              .visibility = default_visibility(arguments[0])};
    std::optional<bool> from_flags;
    for(auto& result: option::table().parse(parse_args, options)) {
        if(!result.has_value())
            continue;
        auto& arg = *result;
        if(arg.id == option::OPT_target && arg.values.size() == 1) {
            from_flags =
                llvm::Triple(llvm::Triple::normalize(arg.values[0])).isWindowsGNUEnvironment();
        }
    }
    if(from_flags)
        return *from_flags;

    auto parsed = clang::driver::ToolChain::getTargetAndModeFromProgramName(arguments[0]);
    return !parsed.TargetPrefix.empty() &&
           llvm::Triple(llvm::Triple::normalize(parsed.TargetPrefix)).isWindowsGNUEnvironment();
}

Toolchain::ToolchainExtract Toolchain::extract_flags(llvm::StringRef file,
                                                     llvm::ArrayRef<const char*> arguments) {
    ToolchainExtract result;

    // LLVM-MinGW's resource headers and libc++ are a matched installation.
    // Let its driver derive those implicit paths instead of forcing clice's
    // resource tree: the injected -resource-dir is dropped from the query
    // (and the key) below. Other targets keep it so the embedded frontend
    // and builtin headers stay version-matched.
    result.preserve_external_resource = uses_windows_gnu_target(arguments);

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

        if(result.preserve_external_resource && arg.id == option::OPT_resource_dir &&
           arg.values.size() == 1 && llvm::StringRef(arg.values[0]) == resource_dir())
            continue;

        result.key += std::to_string(arg.id);
        result.key += '\0';
        /// All unknown options share one id; their identity is the spelling
        /// (the NVCC translation carries `-ccbin=<path>` through here, and
        /// two commands differing only in host compiler must not collide).
        if(arg.id == option::OPT_UNKNOWN) {
            result.key += arg.spelling;
            result.key += '\0';
        }
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

    auto [key, query_args, preserve_external_resource] =
        extract_flags(cmd.source_file, cmd.resolved.flags);

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

    // Preserve a real LLVM-MinGW resource tree. Other external resource paths
    // are replaced with ours to keep the embedded frontend and builtin headers
    // version-matched.
    if(!resource_dir().empty()) {
        llvm::StringRef old_resource_dir;
        for(std::size_t i = 0; i + 1 < new_flags.size(); ++i) {
            if(new_flags[i] == llvm::StringRef("-resource-dir")) {
                old_resource_dir = new_flags[i + 1];
                break;
            }
        }
        bool keep_external =
            preserve_external_resource && llvm::sys::fs::is_directory(old_resource_dir);
        if(!old_resource_dir.empty() && old_resource_dir != resource_dir() && !keep_external) {
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

        auto extract = extract_flags(cmd.source_file, cmd.resolved.flags);
        auto& key = extract.key;
        if(cache.count(key) || failed.count(key) || !seen.try_emplace(key, true).second)
            continue;

        pending.push_back({std::move(key), std::move(extract.query_args), cmd.source_file});
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
