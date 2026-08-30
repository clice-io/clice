#include "command/toolchain.h"

#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "command/argument_parser.h"
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
#include "clang/Driver/Types.h"

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
    execute_async(std::vector<std::string> arguments,
                  bool capture_stdout = false,
                  std::string cwd = {}) {
    kota::process::options opts;
    opts.file = arguments[0];
    opts.args = std::move(arguments);
    opts.cwd = std::move(cwd);
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

kota::task<std::expected<GCCToolchainFlags, std::string>> query_gcc_flags(std::string driver,
                                                                          std::string cwd) {
    auto target = co_await execute_async({driver, "-dumpmachine"}, true, cwd);
    if(!target)
        co_return std::unexpected(std::move(target.error()));

    auto search_dirs = co_await execute_async({driver, "-print-search-dirs"}, true, cwd);
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

/// The temp-file extension a probe input uses for `kind`: clang's own
/// suffix when the kind is a known language, the raw extension itself
/// otherwise (the driver ends up exactly as confused as it would be by the
/// real file — the probe fails the same way the real compile would).
/// The probe working directory, empty when the wanted one does not exist
/// (a stale CDB directory, or a foreign-platform path): probing from the
/// process cwd degrades the answer instead of failing the spawn.
std::string probe_cwd(llvm::StringRef wanted) {
    if(wanted.empty() || !fs::is_directory(wanted)) {
        return {};
    }
    return wanted.str();
}

std::string temp_suffix_for(llvm::StringRef kind) {
    namespace types = clang::driver::types;
    auto type = types::lookupTypeForTypeSpecifier(kind.str().c_str());
    if(type != types::TY_INVALID) {
        /// TY_Nothing ("-x none") has no temp suffix.
        if(const char* suffix = types::getTypeTempSuffix(type)) {
            return suffix;
        }
    }
    return kind.str();
}

/// One probe request: everything query_one() needs, self-contained so warm()
/// can move it into a coroutine frame.
struct QuerySpec {
    /// Driver (+ subcommand) + non-user-content flags; no input among them.
    std::vector<const char*> argv;

    /// Where the temp input is inserted (an index into argv).
    std::size_t slot = 0;

    /// The input language (InputKind value).
    std::string kind;

    CompilerFamily family = CompilerFamily::Unknown;

    /// Probe working directory; empty inherits the process cwd.
    std::string cwd;
};

kota::task<std::expected<std::vector<std::string>, std::string>> query_one(const QuerySpec& spec) {
    if(spec.argv.empty())
        co_return std::unexpected(std::string("Empty arguments"));

    llvm::StringRef driver = spec.argv[0];

    /// Note: The name used to invoke the compiler driver affects its behavior.
    /// For example, `/usr/bin/clang++` is often a symbolic link to
    /// `/usr/lib/llvm-20/bin/clang`. Invoking it as `clang++` enables C++ mode
    /// and links C++ libraries by default, while invoking as `clang` defaults to C mode.
    /// Therefore, never use `realpath` on the initial `driver` name, as that
    /// would lose the context needed for the driver to behave correctly (and break caching).
    llvm::SmallString<128> resolved_path;
    if(!path::is_absolute(driver)) {
        if(driver.contains('/') || driver.contains('\\')) {
            /// Relative with a separator (`./toolchain/clang++`): relative
            /// to the probe working directory, never a PATH lookup.
            if(!spec.cwd.empty()) {
                resolved_path = spec.cwd;
                path::append(resolved_path, driver);
                driver = resolved_path.c_str();
            }
        } else {
            /// A bare name like g++: find it in the env vars.
            auto program = llvm::sys::findProgramByName(driver);
            if(!program)
                co_return std::unexpected(std::format("Cannot find driver: {}", driver.str()));
            resolved_path = *program;
            driver = resolved_path.c_str();
        }
    }

    if(!fs::exists(driver) || !fs::can_execute(driver))
        co_return std::unexpected(
            std::format("Driver {} not found or not executable", driver.str()));

    auto suffix = temp_suffix_for(spec.kind);

    /// Create a file with the kind's suffix, because the real input may not
    /// exist on disk (and a borrowed header must probe as the host's
    /// language, not as its own extension).
    llvm::SmallString<64> src_path;
    if(auto e = fs::createTemporaryFile("query-toolchain", suffix, src_path))
        co_return std::unexpected(std::format("Failed to create temp file: {}", e.message()));
    auto cleanup = llvm::make_scope_exit([&] {
        if(auto e = fs::remove(src_path))
            LOG_ERROR("Fail to remove temporary file: {}", e);
    });

    /// The input sits at the slot recorded from the structured command:
    /// selectors after the slot must not govern it (`clang foo.c -x c++`
    /// compiles foo.c as C).
    llvm::SmallVector<const char*, 256> args;
    args.emplace_back(driver.data());
    args.append(spec.argv.begin() + 1, spec.argv.end());
    args.insert(args.begin() + spec.slot, src_path.c_str());

    std::vector<std::string> cc1_args;

    switch(spec.family) {
        // Query g++ or mingw toolchain info. We detect the target and corresponding
        // gcc toolchain install path as default behavior.
        case CompilerFamily::GCC: {
            auto gcc = co_await query_gcc_flags(driver.str(), spec.cwd);
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

            if(spec.family == CompilerFamily::Zig) {
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

            auto content = co_await execute_async(std::move(exec_args), false, spec.cwd);
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
            /// language so it reports the pipeline the real file gets.
            bool cuda_input = spec.kind == "cuda";
            llvm::StringRef probe_ext = cuda_input ? "cu" : llvm::StringRef(suffix);

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

            auto dryrun = co_await execute_async(std::move(dryrun_args), false, spec.cwd);
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
                    auto gcc = co_await query_gcc_flags(host, spec.cwd);
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
                    /// The structured render spells aliases unaliased
                    /// (--offload-*-only); accept both forms.
                    if(arg == "--cuda-host-only" || arg == "--offload-host-only")
                        selected_host = true;
                    else if(arg == "--cuda-device-only" || arg == "--offload-device-only")
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
                      kota::meta::enum_name(spec.family),
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
    // (render appends the real source file at the end). Also strip module
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
        co_return std::unexpected(std::format("No cc1 args produced for kind {}", spec.kind));

    co_return cc1_args;
}

/// Every component of a search-path-style environment value is absolute and
/// non-empty (an empty component means the cwd).
bool components_all_absolute(llvm::StringRef value, char separator) {
    llvm::SmallVector<llvm::StringRef, 16> parts;
    value.split(parts, separator, -1, /*KeepEmpty=*/true);
    return ranges::all_of(parts, [](llvm::StringRef part) {
        return !part.empty() && path::is_absolute(part);
    });
}

/// Whether every search-path environment variable the compiler consults is
/// absolute — one of the cwd-insensitivity conditions. Computed once: the
/// server never mutates its environment.
bool search_env_all_absolute() {
    const static bool result = [] {
#ifdef _WIN32
        for(const char* name: {"INCLUDE", "LIB", "PATH"}) {
            if(const char* value = std::getenv(name)) {
                if(!components_all_absolute(value, ';'))
                    return false;
            }
        }
#else
        for(const char* name: {"PATH",
                               "CPATH",
                               "C_INCLUDE_PATH",
                               "CPLUS_INCLUDE_PATH",
                               "OBJC_INCLUDE_PATH",
                               "COMPILER_PATH",
                               "LIBRARY_PATH"}) {
            if(const char* value = std::getenv(name)) {
                if(!components_all_absolute(value, ':'))
                    return false;
            }
        }
        /// A single path, not a list.
        if(const char* prefix = std::getenv("GCC_EXEC_PREFIX")) {
            if(!path::is_absolute(prefix))
                return false;
        }
#endif
        return true;
    }();
    return result;
}

/// Options whose value names a filesystem location the driver resolves
/// itself — a relative value ties the probe to the working directory even
/// when it carries no separator (--sysroot=sdk).
bool is_path_taking_option(unsigned id) {
    switch(id) {
        case option::OPT__sysroot_EQ:
        case option::OPT__sysroot:
        case option::OPT_isysroot:
        case option::OPT_B:
        case option::OPT_gcc_toolchain:
        case option::OPT_gcc_install_dir_EQ:
        case option::OPT_cuda_path_EQ:
        case option::OPT_resource_dir:
        case option::OPT_resource_dir_EQ:
        case option::OPT_config:
        case option::OPT_config_system_dir_EQ:
        case option::OPT_config_user_dir_EQ: return true;
        default: return false;
    }
}

bool relative_suspect(llvm::StringRef value) {
    if(value.empty() || path::is_absolute(value)) {
        return false;
    }
    return value.contains('/') || value.contains('\\') || value.starts_with(".");
}

/// Whether the command targets windows-gnu: an explicit target flag wins
/// (the last one, matching the driver's getLastArg); otherwise a
/// target-prefixed driver name (llvm-mingw installs
/// `x86_64-w64-mingw32-clang++`-style wrappers that derive the target
/// implicitly) decides. Parsing resolved aliases, so the legacy `-target`
/// and `=` spellings arrive as the canonical id.
bool uses_windows_gnu_target(const CompileConfig& config) {
    std::optional<bool> from_flags;
    for(auto& arg: config.args) {
        if(arg.opt_id == option::OPT_target && arg.values.size() == 1) {
            from_flags =
                llvm::Triple(llvm::Triple::normalize(arg.values[0])).isWindowsGNUEnvironment();
        }
    }
    if(from_flags)
        return *from_flags;

    auto parsed = clang::driver::ToolChain::getTargetAndModeFromProgramName(config.driver);
    return !parsed.TargetPrefix.empty() &&
           llvm::Triple(llvm::Triple::normalize(parsed.TargetPrefix)).isWindowsGNUEnvironment();
}

/// Which args feed the probe (and its key): everything that may change
/// driver behavior. User-content never does; unknown tokens only as NVCC
/// probe tokens (junk from the CDB must not fail an otherwise good probe).
bool in_probe_view(const Arg& arg, CompilerFamily family) {
    switch(arg.cls) {
        case ArgClass::Semantic:
        case ArgClass::Diagnostics: return true;
        case ArgClass::Unknown:
            return family == CompilerFamily::NVCC && is_nvcc_probe_flag(arg.spelling);
        case ArgClass::UserContent:
        case ArgClass::Codegen:
        case ArgClass::Discarded:
        case ArgClass::Input: return false;
    }
    std::unreachable();
}

}  // namespace

Toolchain::Toolchain(CompilationDatabase& db, std::chrono::steady_clock::duration failed_retry) :
    db(db), failed_retry(failed_retry) {}

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

    /// Windows resolves executable names case-insensitively, and CDBs record
    /// spellings like CL.exe or Clang-Cl.EXE — match lowercased.
    std::string lowered = llvm::sys::path::filename(driver).lower();
    llvm::StringRef name = lowered;
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
    if(arguments.empty()) {
        return std::unexpected(std::string("Empty arguments"));
    }

    QuerySpec spec;
    spec.argv.assign(arguments.begin(), arguments.end());
    spec.slot = spec.argv.size();
    spec.family = driver_family(arguments[0]);

    /// The kind is a clang language name ("cuda", "c++"), the convention
    /// every consumer speaks (`spec.kind == "cuda"`, temp_suffix_for's -x
    /// table). `.cuh` is absent from clang's extension table; a raw
    /// extension only survives when there is no mapping at all.
    auto ext = path::extension(file);
    ext.consume_front(".");
    auto lang = clang::driver::types::lookupTypeForExtension(ext);
    if(ext == "cuh") {
        spec.kind = "cuda";
    } else if(lang != clang::driver::types::TY_INVALID) {
        spec.kind = clang::driver::types::getTypeName(lang);
    } else {
        spec.kind = ext.str();
    }
    for(std::size_t i = 0; i + 1 < arguments.size(); i += 1) {
        if(llvm::StringRef(arguments[i]) == "-x")
            spec.kind = arguments[i + 1];
    }

    std::expected<std::vector<std::string>, std::string> result;
    kota::event_loop loop;
    auto task = [&]() -> kota::task<> {
        result = co_await query_one(spec);
    };
    loop.schedule(task());
    loop.run();
    return result;
}

Toolchain::ProbeKey Toolchain::probe_key(ConfigID id, InputKind input) {
    auto& config = db.config(id);
    ProbeKey out;

    /// cwd-insensitivity is an exemption earned by four orthogonal
    /// conditions; any miss keys (and runs) the probe in the entry
    /// directory. The conditions are independent — an absolute driver does
    /// not excuse a relative CPATH.
    bool known_family = config.family != CompilerFamily::Unknown;

    llvm::StringRef driver = config.driver;
#ifdef _WIN32
    /// Windows resolves bare names against the current directory first
    /// (plus PATHEXT variants) — never exempt.
    bool driver_cwd_free = path::is_absolute(driver);
#else
    /// A bare name resolves through PATH alone (whose components the env
    /// condition below covers); a relative path is cwd-bound.
    bool driver_cwd_free =
        path::is_absolute(driver) || (!driver.contains('/') && !driver.contains('\\'));
#endif

    bool values_clean = true;
    for(auto& arg: config.args) {
        if(!in_probe_view(arg, config.family)) {
            continue;
        }
        if(arg.opt_id == option::OPT_UNKNOWN) {
            llvm::StringRef spelling(arg.spelling);
            if(relative_suspect(spelling.substr(spelling.find('=') + 1))) {
                values_clean = false;
            }
            continue;
        }
        for(llvm::StringRef value: arg.values) {
            if(relative_suspect(value) ||
               (is_path_taking_option(arg.opt_id) && !path::is_absolute(value))) {
                values_clean = false;
            }
        }
    }

    out.cwd_sensitive =
        !(known_family && driver_cwd_free && search_env_all_absolute() && values_clean);

    auto append = [&](llvm::StringRef fragment) {
        out.key += fragment;
        out.key += '\0';
    };

    append(config.driver);
    if(config.subcommand) {
        append(config.subcommand);
    }
    append(input.value ? input.value : "");
    if(out.cwd_sensitive) {
        append(config.directory);
    }

    for(auto& arg: config.args) {
        if(!in_probe_view(arg, config.family)) {
            continue;
        }
        out.key += std::to_string(arg.opt_id);
        out.key += '\0';
        /// All unknown options share one id; their identity is the spelling
        /// (the NVCC translation carries `-ccbin=<path>` through here, and
        /// two commands differing only in host compiler must not collide).
        if(arg.opt_id == option::OPT_UNKNOWN) {
            append(arg.spelling);
        }
        for(const char* value: arg.values) {
            append(value);
        }
    }

    return out;
}

Toolchain::ProbeArgv Toolchain::probe_argv(const CompileConfig& config, bool cwd_sensitive) {
    ProbeArgv out;
    out.argv.push_back(config.driver);
    if(config.subcommand) {
        out.argv.push_back(config.subcommand);
    }

    auto emit = [&](std::string_view fragment) {
        out.argv.push_back(db.strings.save(fragment).data());
    };

    out.slot = out.argv.size();
    for(auto& arg: config.args) {
        if(arg.cls == ArgClass::Input) {
            out.slot = out.argv.size();
            continue;
        }
        if(!in_probe_view(arg, config.family)) {
            continue;
        }
        if(arg.opt_id == option::OPT_UNKNOWN) {
            out.argv.push_back(arg.spelling);
            continue;
        }

        /// A cwd-sensitive probe cannot rely on the working directory for
        /// the in-process driver branches — relative path values absolutize
        /// here (the subprocess branches also run with cwd = directory,
        /// which resolves everything else, e.g. driver config files).
        if(cwd_sensitive) {
            Arg adjusted = arg;
            llvm::SmallVector<const char*, 2> values;
            bool changed = false;
            for(llvm::StringRef value: arg.values) {
                bool pathy =
                    relative_suspect(value) || (is_path_taking_option(arg.opt_id) &&
                                                !path::is_absolute(value) && !value.empty());
                if(pathy) {
                    values.push_back(db.strings.save(path::join(config.directory, value)).data());
                    changed = true;
                } else {
                    values.push_back(value.data());
                }
            }
            if(changed) {
                adjusted.values = values;
                render_arg(adjusted, emit);
                continue;
            }
        }

        render_arg(arg, emit);
    }

    return out;
}

Toolchain::ResolvedID Toolchain::synthesize(ConfigID id, llvm::ArrayRef<const char*> tokens) {
    auto& config = db.config(id);

    Resolved resolved;
    resolved.driver = tokens[0];
    tokens = tokens.drop_front();

    resolved.is_cc1 = ranges::contains(tokens, llvm::StringRef("-cc1"), [](const char* token) {
        return llvm::StringRef(token);
    });

    std::vector<std::string> parse_args;
    parse_args.reserve(tokens.size());
    for(llvm::StringRef token: tokens) {
        if(token != "-cc1") {
            parse_args.push_back(token.str());
        }
    }

    auto parse_options = kota::option::ParseOptions{
        .visibility = resolved.is_cc1 ? static_cast<unsigned>(option::CC1Option)
                                      : family_visibility(config.family)};

    struct Staged {
        std::uint32_t opt_id;
        ArgClass cls;
        const char* spelling = nullptr;
        llvm::SmallVector<const char*, 2> values;
    };

    std::vector<Staged> staged;
    staged.reserve(parse_args.size());
    for(auto& parsed: option::table().parse(parse_args, parse_options)) {
        if(!parsed.has_value()) {
            auto index = parsed.error().index;
            if(index < parse_args.size()) {
                staged.push_back({.opt_id = option::OPT_UNKNOWN,
                                  .cls = ArgClass::Unknown,
                                  .spelling = db.strings.save(parse_args[index]).data()});
            }
            continue;
        }
        auto& arg = *parsed;
        /// -main-file-name is per-input identity; render re-injects it with
        /// the real file's basename. A leftover input token cannot reach
        /// the compile argv (the probe input was erased by exact match; a
        /// canonicalized echo would slip through here).
        if(arg.id == option::OPT_main_file_name || arg.id == option::OPT_INPUT) {
            continue;
        }
        if(arg.id == option::OPT_UNKNOWN) {
            if(arg.index < parse_args.size()) {
                staged.push_back({.opt_id = option::OPT_UNKNOWN,
                                  .cls = ArgClass::Unknown,
                                  .spelling = db.strings.save(parse_args[arg.index]).data()});
            }
            continue;
        }
        Staged local;
        local.opt_id = arg.id;
        for(auto value: arg.values) {
            local.values.push_back(db.strings.save(value).data());
        }
        local.cls = is_user_content_option(arg.id) ? ArgClass::UserContent : ArgClass::Semantic;
        staged.push_back(std::move(local));
    }

    /// Preserve a real LLVM-MinGW resource tree (a matched installation:
    /// resource headers and libc++ belong together). Other external
    /// resource paths are replaced with ours to keep the embedded frontend
    /// and builtin headers version-matched.
    if(!resource_dir().empty()) {
        llvm::StringRef old_resource_dir;
        for(auto& arg: staged) {
            if((arg.opt_id == option::OPT_resource_dir ||
                arg.opt_id == option::OPT_resource_dir_EQ) &&
               arg.values.size() == 1) {
                old_resource_dir = arg.values[0];
                break;
            }
        }
        bool keep_external =
            uses_windows_gnu_target(config) && llvm::sys::fs::is_directory(old_resource_dir);
        if(!old_resource_dir.empty() && old_resource_dir != resource_dir() && !keep_external) {
            for(auto& arg: staged) {
                for(auto& value: arg.values) {
                    llvm::StringRef s(value);
                    if(s.starts_with(old_resource_dir)) {
                        auto replaced =
                            resource_dir().str() + s.substr(old_resource_dir.size()).str();
                        value = db.strings.save(replaced).data();
                    }
                }
            }
        }
    }

    /// Re-attach the config's own user-content flags (-I, -D, -include, ...)
    /// — they never fed the probe. Structured already; no re-parse.
    for(auto& arg: config.args) {
        if(arg.cls == ArgClass::UserContent) {
            Staged local;
            local.opt_id = arg.opt_id;
            local.cls = ArgClass::UserContent;
            local.spelling = arg.spelling;
            local.values.assign(arg.values.begin(), arg.values.end());
            staged.push_back(std::move(local));
        }
    }

    auto* args = db.allocator->Allocate<Arg>(staged.size());
    for(std::size_t i = 0; i < staged.size(); i += 1) {
        args[i] = Arg{.opt_id = staged[i].opt_id,
                      .cls = staged[i].cls,
                      .spelling = staged[i].spelling,
                      .values = db.persist_strings(staged[i].values)};
    }
    resolved.args = {args, staged.size()};

    resolved_configs.push_back(resolved);
    return static_cast<ResolvedID>(resolved_configs.size() - 1);
}

std::expected<Toolchain::ResolvedID, std::string> Toolchain::resolve(ConfigID id, InputKind input) {
    auto synth_key = std::pair{static_cast<std::uint32_t>(id), input.value};
    if(auto it = synth_cache.find(synth_key); it != synth_cache.end()) {
        return it->second;
    }

    auto pk = probe_key(id, input);
    auto it = probes.find(pk.key);
    if(it == probes.end()) {
        if(auto failed_it = failed.find(pk.key); failed_it != failed.end()) {
            if(std::chrono::steady_clock::now() - failed_it->second.second < failed_retry) {
                return std::unexpected(failed_it->second.first);
            }
            // Cooldown over: the failure may have been transient — retry
            // the real query (cf. CrashBudget's bounded-burn revival).
            failed.erase(failed_it);
        }

        auto& config = db.config(id);
        LOG_WARN("Toolchain probe miss: driver={} kind={}", config.driver, input.value);

        QuerySpec spec;
        auto argv = probe_argv(config, pk.cwd_sensitive);
        spec.argv = std::move(argv.argv);
        spec.slot = argv.slot;
        spec.kind = input.value;
        spec.family = config.family;
        spec.cwd = probe_cwd(pk.cwd_sensitive ? config.directory : db.workspace_root);

        std::expected<std::vector<std::string>, std::string> result;
        kota::event_loop loop;
        auto task = [&]() -> kota::task<> {
            result = co_await query_one(spec);
        };
        loop.schedule(task());
        loop.run();

        if(!result) {
            failed.try_emplace(pk.key, std::pair{result.error(), std::chrono::steady_clock::now()});
            return std::unexpected(std::move(result.error()));
        }

        llvm::SmallVector<const char*, 64> saved;
        saved.reserve(result->size());
        for(auto& token: *result) {
            saved.push_back(db.strings.save(token).data());
        }
        it = probes.try_emplace(pk.key, std::move(saved)).first;
    }

    auto resolved_id = synthesize(id, it->second);
    synth_cache.try_emplace(synth_key, resolved_id);
    return resolved_id;
}

void Toolchain::warm(llvm::ArrayRef<std::pair<ConfigID, InputKind>> pairs) {
    struct Pending {
        std::string key;
        QuerySpec spec;
    };

    llvm::StringMap<bool> seen;
    std::vector<Pending> pending;

    for(auto& [id, input]: pairs) {
        auto pk = probe_key(id, input);
        if(probes.contains(pk.key)) {
            continue;
        }
        if(auto failed_it = failed.find(pk.key); failed_it != failed.end()) {
            // Same expiry as resolve(): a cooled-down failure retries
            // through this warm instead of being skipped forever.
            if(std::chrono::steady_clock::now() - failed_it->second.second < failed_retry) {
                continue;
            }
            failed.erase(failed_it);
        }
        if(!seen.try_emplace(pk.key, true).second) {
            continue;
        }

        auto& config = db.config(id);
        auto argv = probe_argv(config, pk.cwd_sensitive);
        QuerySpec spec;
        spec.argv = std::move(argv.argv);
        spec.slot = argv.slot;
        spec.kind = input.value;
        spec.family = config.family;
        spec.cwd = probe_cwd(pk.cwd_sensitive ? config.directory : db.workspace_root);
        pending.push_back({std::move(pk.key), std::move(spec)});
    }

    if(pending.empty()) {
        return;
    }

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
    auto make_task = [](Pending p) -> kota::task<QueryOutcome> {
        auto result = co_await query_one(p.spec);
        co_return QueryOutcome{std::move(p.key), std::move(result)};
    };

    kota::small_vector<QueryOutcome> outcomes;

    kota::event_loop loop;
    auto run = [&]() -> kota::task<> {
        std::vector<kota::task<QueryOutcome>> tasks;
        tasks.reserve(pending.size());
        for(auto& p: pending) {
            tasks.push_back(make_task(std::move(p)));
        }

        outcomes = co_await kota::when_all(std::move(tasks));
    };
    loop.schedule(run());
    loop.run();

    std::size_t succeeded = 0;
    for(auto& o: outcomes) {
        if(!o.result) {
            LOG_ERROR("Toolchain query failed: {}", o.result.error());
            failed.try_emplace(
                std::move(o.key),
                std::pair{std::move(o.result.error()), std::chrono::steady_clock::now()});
            continue;
        }

        llvm::SmallVector<const char*, 64> saved;
        saved.reserve(o.result->size());
        for(auto& token: *o.result) {
            saved.push_back(db.strings.save(token).data());
        }
        probes.try_emplace(std::move(o.key), std::move(saved));
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
