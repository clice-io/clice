module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument_parser;
import :command.driver_report;
import :command.nvcc;
import :command.toolchain;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :vfs.path;

namespace clice {

namespace {

namespace ranges = std::ranges;

/// The driver followed by the arguments, from -cc1 on, of the clang job
/// clang's driver builds for `driver_arguments`.
std::expected<std::vector<std::string>, std::string>
    query_driver(llvm::ArrayRef<std::string> driver_arguments) {
    /// FIXME: collect diagnostic here ...
    clang::DiagnosticOptions options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    options,
                                    new clang::IgnoringDiagConsumer());

    llvm::SmallVector<const char*, 256> list;
    list.emplace_back(driver_arguments.front().c_str());
    list.emplace_back("-fsyntax-only");
    for(auto& argument: driver_arguments.drop_front()) {
        list.emplace_back(argument.c_str());
    }
    llvm::ArrayRef<const char*> arguments = list;

    /// Note that clang use the `DriverExecutable` to determine the driver mode when
    /// --driver-mode is not found in the arguments, and `TargetTriple` is used when
    /// non --target argument is found in the arguments list. See
    /// `clang::driver::BuildCompilation`. We use default arguments because we will
    /// inject related commands before querying.
    clang::driver::Driver driver(/*DriverExecutable=*/arguments[0],
                                 /*TargetTriple=*/llvm::sys::getDefaultTargetTriple(),
                                 /*Diags=*/engine,
                                 /*Title=*/"clang LLVM compiler",
                                 /*VFS=*/llvm::makeIntrusiveRefCnt<vfs::View>());
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

    std::vector<std::string> cc1_args{arguments[0]};
    cc1_args.insert(cc1_args.end(), cmd->getArguments().begin(), cmd->getArguments().end());
    return cc1_args;
}

/// Asks a GCC-style driver how it compiles `args`, the driver (its first
/// `driver_tokens`: two for `zig cc`) followed by its flags and the input: `-E -dM
/// -v` has it preprocess the input verbosely, printing the job it runs and
/// its predefined macros. A clang job that then fails still answers: the
/// command it runs is what clice compiles, and fails the same way there (an
/// unknown `--target`). A GCC job answers only when it succeeds: GCC prints
/// its macros and LIBRARY_PATH only then.
kota::task<std::expected<DriverReport, std::string>> ask_driver(llvm::ArrayRef<std::string> args,
                                                                std::size_t driver_tokens,
                                                                std::string cwd) {
    std::vector<std::string> argv(args.begin(), args.begin() + driver_tokens);
    argv.insert(argv.end(), {"-E", "-dM", "-v"});
    argv.insert(argv.end(), args.begin() + driver_tokens, args.end());
    auto output = co_await execute(std::move(argv), std::move(cwd));
    if(!output) {
        co_return std::unexpected(std::move(output.error()));
    }
    auto report = parse_driver_report(output->out, output->err);
    if(report.clang_job.empty() && !output->failure.empty()) {
        auto last_line = llvm::StringRef(output->err).rtrim();
        last_line = last_line.substr(last_line.rfind('\n') + 1);
        co_return std::unexpected(std::format("{}: {}", output->failure, last_line.str()));
    }
    co_return report;
}

/// GCC's two-core tuning form, `-mcpu=cortex-a72.cortex-a53`, as the first
/// core alone: clang knows no such name, and the first core's architecture
/// is the code's. The features after the cores keep their dots
/// (`-mcpu=cortex-m7+nofp.dp`).
std::string first_core(llvm::StringRef arg) {
    if(!arg.starts_with("-mcpu=")) {
        return arg.str();
    }
    auto features = arg.find('+');
    return (arg.substr(0, features).split('.').first + arg.substr(features)).str();
}

/// A universal build (`-arch arm64 -arch x86_64`) compiles once for each
/// architecture, and a driver preprocesses for one only: clice reads the
/// code for the first.
void keep_first_arch(std::vector<std::string>& args) {
    bool seen = false;
    for(std::size_t i = 0; i + 1 < args.size();) {
        if(args[i] == "-arch") {
            if(seen) {
                args.erase(args.begin() + i, args.begin() + i + 2);
                continue;
            }
            seen = true;
        }
        i += 1;
    }
}

/// The driver arguments that set clang up as GCC set itself up: GCC's
/// target, none of clang's own idea of the system headers (GCC's search
/// list replaces them), and the options GCC's driver gave its cc1 job that
/// clang's driver knows.
void pin_to_gcc(const DriverReport& report, std::vector<std::string>& args) {
    args.insert(args.end(),
                {"--target=" + report.target, "-nostdinc", "-resource-dir", resource_dir().str()});
    auto parse_options =
        kota::option::ParseOptions{.visibility = family_visibility(CompilerFamily::GCC)};
    for(auto& parsed: option::table().parse(report.gcc_options, parse_options)) {
        if(parsed && parsed->id != option::OPT_UNKNOWN) {
            args.push_back(first_core(report.gcc_options[parsed->index]));
        }
    }
}

/// Puts GCC's header search list on a cc1 job pinned to GCC, with clang's
/// builtin headers in place of GCC's own. GCC's are its versions of the
/// same headers, which clang cannot parse, and clang's would reach them
/// through #include_next (clang's stdatomic.h defers to a later one).
void append_gcc_search_list(const DriverReport& report, std::vector<std::string>& cc1) {
    for(std::size_t i = 0; i < report.search_dirs.size(); i += 1) {
        auto& dir = report.search_dirs[i];
        if(report.private_dir == i) {
            cc1.insert(cc1.end(), {"-internal-isystem", path::join(resource_dir(), "include")});
            continue;
        }
        cc1.insert(cc1.end(),
                   {dir.framework ? "-internal-iframework" : "-internal-isystem", dir.path});
    }
}

/// Drops the CPU and ABI names clang's target does not know from a cc1
/// job: GCC accepts names of its own (`-mtune=intel`, `-mabi=lp64` on
/// Darwin), a newer external clang knows newer CPUs, and any one of them
/// fails the whole compile. The target's defaults stand in.
void drop_unknown_target_names(std::vector<std::string>& cc1) {
    auto triple = std::ranges::find(cc1, "-triple");
    if(triple == cc1.end()) {
        return;
    }
    clang::DiagnosticOptions diagnostic_options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    diagnostic_options,
                                    new clang::IgnoringDiagConsumer());
    clang::TargetOptions target_options;
    target_options.Triple = *(triple + 1);
    llvm::IntrusiveRefCntPtr<clang::TargetInfo> target(
        clang::TargetInfo::CreateTargetInfo(engine, target_options));
    if(!target) {
        return;
    }
    auto known = [&](llvm::StringRef option, llvm::StringRef name) {
        if(option == "-target-cpu") {
            return target->isValidCPUName(name);
        }
        if(option == "-tune-cpu") {
            return target->isValidTuneCPUName(name);
        }
        return option != "-target-abi" || target->setABI(name.str());
    };
    for(std::size_t i = 0; i + 1 < cc1.size();) {
        if(!known(cc1[i], cc1[i + 1])) {
            cc1.erase(cc1.begin() + i, cc1.begin() + i + 2);
            continue;
        }
        i += 1;
    }
}

/// The probe working directory, empty when the wanted one does not exist
/// (a stale CDB directory, or a foreign-platform path): probing from the
/// process cwd degrades the answer instead of failing the spawn.
std::string probe_cwd(llvm::StringRef wanted) {
    if(wanted.empty() || !vfs::is_directory(wanted)) {
        return {};
    }
    return wanted.str();
}

/// The temp-file extension a probe input uses for `kind`: clang's own
/// suffix when the kind is a known language, the raw extension itself
/// otherwise (the driver ends up exactly as confused as it would be by the
/// real file — the probe fails the same way the real compile would).
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

    if(!vfs::exists(driver) || !llvm::sys::fs::can_execute(driver))
        co_return std::unexpected(
            std::format("Driver {} not found or not executable", driver.str()));

    auto suffix = temp_suffix_for(spec.kind);

    /// Create a file with the kind's suffix, because the real input may not
    /// exist on disk (and a borrowed header must probe as the host's
    /// language, not as its own extension).
    auto src_path = vfs::temp_file("query-toolchain", suffix);
    if(!src_path)
        co_return std::unexpected(
            std::format("Failed to create temp file: {}", src_path.error().message()));
    auto cleanup = llvm::make_scope_exit([&] {
        if(auto e = vfs::remove(*src_path))
            LOG_ERROR("Fail to remove temporary file: {}", e);
    });

    /// The input sits at the slot recorded from the structured command:
    /// selectors after the slot must not govern it (`clang foo.c -x c++`
    /// compiles foo.c as C).
    std::vector<std::string> args(spec.argv.begin(), spec.argv.end());
    args[0] = driver.str();
    args.insert(args.begin() + spec.slot, *src_path);
    bool names_standard =
        ranges::any_of(args, [](llvm::StringRef arg) { return arg.starts_with("-std="); });

    std::expected<std::vector<std::string>, std::string> queried;
    switch(spec.family) {
        case CompilerFamily::MSVC:
        case CompilerFamily::ClangCL: {
            /// When clang in cl mode, the target will be set to windows-msvc automatically.
            /// We don't need to add extra flag.
            // Before C++20, clang defers template bodies on MSVC targets so that
            // code written for MSVC's lookup rules compiles, but a deferred body has
            // no AST: every feature inside it goes blind. Placed before the
            // command's own flags, this one keeps that default out of the cc1
            // command and leaves deferral to a command asking for it
            // (`-fdelayed-template-parsing`, clang-cl's `/Zc:twoPhase-`), which
            // every compile of the file then follows.
            std::vector<std::string> msvc_args = {args[0],
                                                  "--driver-mode=cl",
                                                  "-fno-delayed-template-parsing"};
            msvc_args.insert(msvc_args.end(), args.begin() + 1, args.end());
            queried = query_driver(msvc_args);
            break;
        }

        // Query nvcc by asking for its compilation pipeline: the dryrun
        // names the toolkit root, the host compiler and the macros nvcc
        // injects — none of which clang can derive on its own. The host
        // compiler is then asked as a GCC-style driver is, with the
        // command's sysroot, and our own driver builds the cc1 command, in
        // CUDA mode for a CUDA input.
        case CompilerFamily::NVCC: {
            /// nvcc only offloads CUDA-language inputs; a host-language
            /// entry (`nvcc -c foo.cpp`, or an explicit `-x c++`) compiles
            /// in a single host step. The dryrun probe follows the effective
            /// language so it reports the pipeline the real file gets.
            bool cuda_input = spec.kind == "cuda";
            llvm::StringRef probe_ext = cuda_input ? "cu" : llvm::StringRef(suffix);

            auto nvcc_probe = vfs::temp_file("query-toolchain", probe_ext);
            if(!nvcc_probe)
                co_return std::unexpected(
                    std::format("Failed to create temp file: {}", nvcc_probe.error().message()));
            auto nvcc_cleanup = llvm::make_scope_exit([&] {
                if(auto e = vfs::remove(*nvcc_probe))
                    LOG_ERROR("Fail to remove temporary file: {}", e);
            });

            std::vector<std::string> dryrun_args = {driver.str(), "--dryrun", "-c", *nvcc_probe};
            for(llvm::StringRef arg: args) {
                if(is_nvcc_probe_flag(arg))
                    dryrun_args.push_back(arg.str());
            }

            auto dryrun = co_await execute(std::move(dryrun_args), spec.cwd);
            if(!dryrun)
                co_return std::unexpected(std::move(dryrun.error()));
            if(!dryrun->failure.empty())
                co_return std::unexpected(std::move(dryrun->failure));

            auto info = parse_nvcc_dryrun(dryrun->err);
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
                    if(vfs::exists(candidate) && llvm::sys::fs::can_execute(candidate)) {
                        resolved_host = std::move(candidate);
                        break;
                    }
                }
                if(resolved_host.empty()) {
                    auto sibling = path::join(path::parent_path(driver), host);
                    if(vfs::exists(sibling) && llvm::sys::fs::can_execute(sibling))
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

            auto host_family = Toolchain::driver_family(host);
            if(host_family == CompilerFamily::MSVC || host_family == CompilerFamily::ClangCL) {
                co_return std::unexpected(std::format("Unsupported nvcc host compiler: {}", host));
            }
            /// nvcc hands its host compiler a CUDA input as C++. The host's
            /// headers follow the sysroot the command gives it
            /// (`-Xcompiler --sysroot=<dir>`).
            std::vector<std::string> host_probe = {host, "-x", cuda_input ? "c++" : spec.kind};
            for(llvm::StringRef arg: llvm::ArrayRef(args).drop_front()) {
                if(arg.starts_with("--sysroot=")) {
                    host_probe.push_back(arg.str());
                }
            }
            host_probe.push_back(*src_path);
            auto host_report = co_await ask_driver(host_probe, 1, spec.cwd);
            if(!host_report) {
                co_return std::unexpected(std::move(host_report.error()));
            }
            if(host_report->clang_job.empty() && !host_report->gcc_job) {
                co_return std::unexpected(std::format("Unsupported nvcc host compiler: {}", host));
            }
            /// A clang host needs no pin: the in-process driver already
            /// defaults to the running machine's triple.
            bool gcc_host = host_report->gcc_job;

            std::vector<std::string> cuda_args = {host, "-fno-delayed-template-parsing"};
            if(gcc_host) {
                pin_to_gcc(*host_report, cuda_args);
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
                    /// The structured render spells aliases unaliased:
                    /// --cuda-host-only arrives as --offload-host-only.
                    if(arg == "--offload-host-only")
                        selected_host = true;
                    else if(arg == "--offload-device-only")
                        selected_host = false;
                }
                bool host_view = selected_host.value_or(false);
                if(!selected_host.has_value())
                    cuda_args.push_back("--cuda-device-only");

                for(auto& define: host_view ? info->host_defines : info->device_defines)
                    cuda_args.push_back("-D" + define);

                if(!names_standard && !info->cpp_dialect.empty())
                    cuda_args.push_back("-std=" + info->cpp_dialect);

                /// An `-arch=` probe token is an explicit selection too: its
                /// resolution replaces it in place below.
                if(!contains_prefix({"--offload-arch=", "-arch="}) && !info->default_arch.empty())
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

            queried = query_driver(cuda_args);
            if(queried && gcc_host) {
                append_gcc_search_list(*host_report, *queried);
            }
            break;
        }

        // Every other driver is GCC-style: asked how it compiles the input,
        // it shows its job, and the job tells clang underneath (Apple clang,
        // zig, emcc, a `c++` that is clang) from GCC.
        default: {
            keep_first_arch(args);
            auto report =
                co_await ask_driver(args, spec.family == CompilerFamily::Zig ? 2 : 1, spec.cwd);
            if(!report) {
                co_return std::unexpected(std::move(report.error()));
            }
            if(!report->clang_job.empty()) {
                queried = std::move(report->clang_job);
                // As for cl: an MSVC target's default template deferral
                // leaves deferred bodies without an AST, so it stays only
                // when the command asks for it.
                if(!ranges::contains(args, "-fdelayed-template-parsing")) {
                    std::erase(*queried, "-fdelayed-template-parsing");
                }
                if(!names_standard && !report->standard.empty()) {
                    queried->push_back("-std=" + report->standard);
                }
                break;
            }
            // Neither job: an input the driver only passes on (assembler),
            // or a driver that is neither GCC nor clang (Intel's icc).
            if(!report->gcc_job) {
                queried.emplace();
                break;
            }
            std::vector<std::string> gcc_args = {args[0]};
            pin_to_gcc(*report, gcc_args);
            for(llvm::StringRef arg: llvm::ArrayRef(args).drop_front()) {
                gcc_args.push_back(first_core(arg));
            }
            if(!names_standard && !report->standard.empty()) {
                gcc_args.push_back("-std=" + report->standard);
            }
            queried = query_driver(gcc_args);
            if(queried) {
                append_gcc_search_list(*report, *queried);
            }
            break;
        }
    }
    if(!queried)
        co_return std::move(queried);
    drop_unknown_target_names(*queried);

    // Strip the temporary probe file so results contain no input path
    // (render appends the real source file at the end), and the module
    // output, which clice manages itself (the driver derives
    // -fmodule-output=<probe>.pcm for a module unit, naming the deleted temp
    // file). The probe path uses an exact match: all supported drivers echo
    // input paths verbatim, without canonicalizing them.
    std::erase_if(*queried, [&](const std::string& arg) {
        llvm::StringRef s(arg);
        return s == *src_path || s.starts_with("-fmodule-output");
    });

    if(queried->empty())
        co_return std::unexpected(std::format("No cc1 args produced for kind {}", spec.kind));

    co_return std::move(queried);
}

/// Every component of a search-path-style environment value is absolute and
/// non-empty (an empty component means the cwd).
bool components_all_absolute(llvm::StringRef value) {
    llvm::SmallVector<llvm::StringRef, 16> parts;
    value.split(parts, path::list_separator, -1, /*KeepEmpty=*/true);
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
                if(!components_all_absolute(value))
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
                if(!components_all_absolute(value))
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

/// Whether an option clang does not know may name a path relative to the
/// working directory: nothing tells, so any relative-looking value does.
bool relative_suspect(llvm::StringRef value) {
    if(value.empty() || path::is_absolute(value)) {
        return false;
    }
    return value.contains('/') || value.contains('\\') || value.starts_with(".");
}

/// Whether option `id` adds a directory to the header search list.
bool names_search_dir(unsigned id) {
    switch(id) {
        case option::OPT_I:
        case option::OPT_isystem:
        case option::OPT_idirafter:
        case option::OPT_internal_isystem:
        case option::OPT_internal_externc_isystem: return true;
        default: return false;
    }
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
/// driver behavior. User-content never does, nor does the command's
/// action; unknown tokens feed it only as NVCC probe tokens (junk from the
/// CDB must not fail an otherwise good probe).
bool in_probe_view(const Arg& arg, CompilerFamily family) {
    switch(arg.cls) {
        case ArgClass::Semantic: {
            // The probe picks its own action; the command's (-fsyntax-only)
            // would go unused, an error under -Werror.
            auto opt = option::table().option(arg.opt_id);
            return !opt || !opt->matches(option::OPT_Action_Group);
        }
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
    /// table).
    spec.kind = extension_language(file, /*cxx=*/false);
    for(std::size_t i = 0; i + 1 < arguments.size(); i += 1) {
        if(llvm::StringRef(arguments[i]) == "-x")
            spec.kind = arguments[i + 1];
    }

    auto [ended] = kota::run(query_one(spec));
    return std::move(*ended);
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

    /// A known option's path values were anchored when the command was
    /// read (names_path); only an unknown one can still hold a
    /// relative path.
    bool values_clean = true;
    for(auto& arg: config.args) {
        if(arg.opt_id != option::OPT_UNKNOWN || !in_probe_view(arg, config.family)) {
            continue;
        }
        llvm::StringRef spelling(arg.spelling);
        if(relative_suspect(spelling.substr(spelling.find('=') + 1))) {
            values_clean = false;
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

Toolchain::ProbeArgv Toolchain::probe_argv(const CompileConfig& config) {
    ProbeArgv out;
    out.argv.push_back(config.driver);
    if(config.subcommand) {
        out.argv.push_back(config.subcommand);
    }

    auto emit = [&](llvm::StringRef fragment) {
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
        // clang reads a bare -flto, and GCC's -flto=auto, as -flto=full,
        // which GCC rejects.
        if(arg.opt_id == option::OPT_flto_EQ && arg.values.size() == 1 &&
           llvm::StringRef(arg.values[0]) == "full") {
            out.argv.push_back("-flto");
            continue;
        }
        render_driver_arg(arg, config.family, emit);
    }

    return out;
}

Toolchain::ResolvedID Toolchain::synthesize(ConfigID id, llvm::ArrayRef<const char*> tokens) {
    auto& config = db.config(id);

    Resolved resolved;
    resolved.driver = tokens[0];

    /// The PCM clice builds is the BMI the importers of the real build read:
    /// a reduced one drops what the global module fragment declares and the
    /// purview never names, partial specializations among them. --precompile
    /// writes a full BMI to -o whatever the driver asks for, and a reduced
    /// one only to a module output the command names.
    auto has = [&](unsigned id) {
        return ranges::any_of(config.args, [&](const Arg& arg) { return arg.opt_id == id; });
    };
    bool full_bmi = has(option::OPT__precompile) && !has(option::OPT_fmodule_output) &&
                    !has(option::OPT_fmodule_output_EQ);

    /// The driver, then -cc1.
    std::vector<std::string> parse_args(tokens.begin() + 2, tokens.end());
    auto parse_options = kota::option::ParseOptions{.visibility = option::CC1Option};

    std::vector<LocalArg> staged;
    for(auto& parsed: option::table().parse(parse_args, parse_options)) {
        /// -main-file-name is per-input identity; render re-injects it with
        /// the real file's basename. A leftover input token cannot reach
        /// the compile argv (the probe input was erased by exact match; a
        /// canonicalized echo would slip through here).
        if(parsed &&
           (parsed->id == option::OPT_main_file_name || parsed->id == option::OPT_INPUT)) {
            continue;
        }
        if(parsed && parsed->id == option::OPT_fmodules_reduced_bmi && full_bmi) {
            continue;
        }
        staged.push_back(db.local_arg(parsed, parse_args));
    }

    /// Re-attach the config's own user-content flags (-I, -D, -include, ...)
    /// — they never fed the probe. Structured already; no re-parse.
    for(auto& arg: config.args) {
        if(arg.cls == ArgClass::UserContent) {
            staged.push_back({
                .opt_id = arg.opt_id,
                .cls = arg.cls,
                .spelling = arg.spelling,
                .values = {arg.values.begin(), arg.values.end()}
            });
        }
    }

    /// Preserve a real LLVM-MinGW resource tree (a matched installation:
    /// resource headers and libc++ belong together). Otherwise the external
    /// resource dir and its builtin headers are replaced with ours to keep
    /// them version-matched with the embedded frontend. Everything else the
    /// driver derived from its resource dir stays external — sanitizer
    /// ignorelists under share/, runtime libraries under lib/: the driver
    /// verified those files exist there, our tree ships include/ alone, and
    /// cc1 aborts the process on an ignorelist it cannot open. Builtin
    /// headers of another clang that a search directory names outside that
    /// tree — the command's `-isystem <clang>/lib/clang/23/include`, zig's
    /// lib/include — become ours too.
    if(!resource_dir().empty()) {
        llvm::StringRef old_resource_dir;
        for(auto& arg: staged) {
            if(arg.opt_id == option::OPT_resource_dir && arg.values.size() == 1) {
                // A trailing separator on the command's -resource-dir reaches
                // cc1 verbatim while the derived paths append without doubling it.
                old_resource_dir = llvm::StringRef(arg.values[0]).rtrim("/\\");
                break;
            }
        }
        bool keep_external = uses_windows_gnu_target(config) && vfs::is_directory(old_resource_dir);
        bool replace_tree = !old_resource_dir.empty() && old_resource_dir != resource_dir();
        auto builtin_headers = path::join(resource_dir(), "include");
        if(!keep_external) {
            // The remainder below the resource dir when ours ships it (the
            // resource dir itself or its builtin headers), separators dropped.
            auto shipped = [](llvm::StringRef rest) -> std::optional<llvm::StringRef> {
                if(!rest.empty() && rest.front() != '/' && rest.front() != '\\') {
                    return std::nullopt;
                }
                rest = rest.ltrim("/\\");
                if(rest.empty() || rest == "include" || rest.starts_with("include/") ||
                   rest.starts_with("include\\")) {
                    return rest;
                }
                return std::nullopt;
            };
            for(auto& arg: staged) {
                for(auto& value: arg.values) {
                    llvm::StringRef rest(value);
                    if(replace_tree && rest.consume_front(old_resource_dir)) {
                        if(auto tail = shipped(rest)) {
                            auto replaced = tail->empty() ? resource_dir().str()
                                                          : path::join(resource_dir(), *tail);
                            value = db.strings.save(replaced).data();
                        }
                    } else if(names_search_dir(arg.opt_id) &&
                              llvm::StringRef(value).rtrim("/\\") != builtin_headers &&
                              vfs::exists(path::join(value, "__stddef_max_align_t.h"))) {
                        value = db.strings.save(builtin_headers).data();
                    }
                }
            }
        }
    }

    resolved.args = db.persist(staged);

    resolved_configs.push_back(resolved);
    return static_cast<ResolvedID>(resolved_configs.size() - 1);
}

struct Toolchain::ProbeAdmission {
    enum class Kind : std::uint8_t { Hit, CoolingDown, Ready };

    Kind kind;
    std::string key;
    /// Ready: the query to run.
    QuerySpec spec;
    /// CoolingDown: the cached failure.
    std::string error;
};

Toolchain::ProbeAdmission Toolchain::admit_probe(ConfigID id, InputKind input) {
    auto pk = probe_key(id, input);
    ProbeAdmission admission{.kind = ProbeAdmission::Kind::Hit, .key = std::move(pk.key)};
    if(probes.contains(admission.key)) {
        return admission;
    }
    if(auto failed_it = failed.find(admission.key); failed_it != failed.end()) {
        if(std::chrono::steady_clock::now() - failed_it->second.second < failed_retry) {
            admission.kind = ProbeAdmission::Kind::CoolingDown;
            admission.error = failed_it->second.first;
            return admission;
        }
        // Cooldown over: the failure may have been transient — retry the
        // real query (cf. BlameBudget's bounded-burn revival).
        failed.erase(failed_it);
    }

    auto& config = db.config(id);
    auto argv = probe_argv(config);
    admission.kind = ProbeAdmission::Kind::Ready;
    admission.spec.argv = std::move(argv.argv);
    admission.spec.slot = argv.slot;
    admission.spec.kind = input.value;
    admission.spec.family = config.family;
    admission.spec.cwd =
        probe_cwd(pk.cwd_sensitive ? config.directory : llvm::StringRef(db.workspace_root));
    return admission;
}

void Toolchain::commit_probe(llvm::StringRef key,
                             const std::expected<std::vector<std::string>, std::string>& result) {
    if(!result) {
        failed.try_emplace(key, std::pair{result.error(), std::chrono::steady_clock::now()});
        return;
    }
    llvm::SmallVector<const char*, 64> saved;
    saved.reserve(result->size());
    for(auto& token: *result) {
        saved.push_back(db.strings.save(token).data());
    }
    probes.try_emplace(key, std::move(saved));
}

std::expected<Toolchain::ResolvedID, std::string> Toolchain::resolve(ConfigID id, InputKind input) {
    auto synth_key = std::pair{static_cast<std::uint32_t>(id), input.value};
    if(auto it = synth_cache.find(synth_key); it != synth_cache.end()) {
        return it->second;
    }

    auto admission = admit_probe(id, input);
    if(admission.kind == ProbeAdmission::Kind::CoolingDown) {
        return std::unexpected(std::move(admission.error));
    }
    if(admission.kind == ProbeAdmission::Kind::Ready) {
        LOG_WARN("Toolchain probe miss: driver={} kind={}", db.config(id).driver, input.value);

        auto [ended] = kota::run(query_one(admission.spec));
        auto& result = *ended;
        commit_probe(admission.key, result);
        if(!result) {
            return std::unexpected(std::move(result.error()));
        }
    }

    auto resolved_id = synthesize(id, probes.find(admission.key)->second);
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
        auto admission = admit_probe(id, input);
        if(admission.kind != ProbeAdmission::Kind::Ready ||
           !seen.try_emplace(admission.key, true).second) {
            continue;
        }
        pending.push_back({std::move(admission.key), std::move(admission.spec)});
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
    auto make_task = [](Pending p, kota::semaphore& drivers) -> kota::task<QueryOutcome> {
        auto running = co_await drivers.scoped_acquire();
        auto result = co_await query_one(p.spec);
        co_return QueryOutcome{std::move(p.key), std::move(result)};
    };

    auto query_all = [&]() -> kota::task<kota::small_vector<QueryOutcome>> {
        // A database whose commands key thousands of probes would otherwise
        // run thousands of drivers at once.
        kota::semaphore drivers(kota::sys::parallelism());
        std::vector<kota::task<QueryOutcome>> tasks;
        tasks.reserve(pending.size());
        for(auto& p: pending) {
            tasks.push_back(make_task(std::move(p), drivers));
        }

        co_return co_await kota::when_all(std::move(tasks));
    };
    auto [outcomes] = kota::run(query_all());

    std::size_t succeeded = 0;
    for(auto& o: *outcomes) {
        if(o.result) {
            succeeded += 1;
        } else {
            LOG_ERROR("Toolchain query failed: {}", o.result.error());
        }
        commit_probe(o.key, o.result);
    }

    LOG_INFO("Toolchain cache warmed: {} succeeded, {} failed", succeeded, total - succeeded);
}

}  // namespace clice
