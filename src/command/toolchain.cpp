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

/// A cc1 job as a driver runs it: the program, `-cc1`, then its arguments.
using Job = std::vector<std::string>;

/// The job clang's own driver builds for `args`: a driver, its flags and
/// the input.
std::expected<Job, std::string> query_driver(llvm::ArrayRef<std::string> args) {
    /// FIXME: collect diagnostic here ...
    clang::DiagnosticOptions options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    options,
                                    new clang::IgnoringDiagConsumer());

    llvm::SmallVector<const char*, 256> list;
    list.emplace_back(args.front().c_str());
    list.emplace_back("-fsyntax-only");
    for(auto& argument: args.drop_front()) {
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

    Job job{arguments[0]};
    job.insert(job.end(), cmd->getArguments().begin(), cmd->getArguments().end());
    return job;
}

/// Asks a GCC-style driver how it compiles `args`, the driver (its first
/// `driver_tokens`: two for `zig cc`) followed by its flags and the input:
/// `-E -dM -v` has it preprocess the input verbosely, printing the job it
/// runs and its predefined macros. A clang job that then fails still
/// answers: the command it runs is what clice compiles, and fails the same
/// way there (an unknown `--target`). A GCC job answers only when it
/// succeeds: GCC prints its macros and LIBRARY_PATH only then.
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

/// The job clang's own driver builds for `args` (a driver, its flags and
/// the input) set up as GCC set itself up: GCC's target, the options GCC's
/// driver gave its cc1 job that clang's driver knows, and GCC's header
/// search list in place of clang's own idea of the system headers. clang's
/// builtin headers take the place of GCC's own: those are GCC's versions of
/// the same headers, which clang cannot parse, and clang's would reach them
/// through #include_next (clang's stdatomic.h defers to a later one).
std::expected<Job, std::string> query_as_gcc(const DriverReport& report,
                                             llvm::ArrayRef<std::string> args) {
    std::vector<std::string> clang_args = {args[0],
                                           "--target=" + report.target,
                                           "-nostdinc",
                                           "-resource-dir",
                                           resource_dir().str()};
    auto parse_options =
        kota::option::ParseOptions{.visibility = family_visibility(CompilerFamily::GCC)};
    for(auto& parsed: option::table().parse(report.gcc_options, parse_options)) {
        if(parsed && parsed->id != option::OPT_UNKNOWN) {
            clang_args.push_back(first_core(report.gcc_options[parsed->index]));
        }
    }
    for(llvm::StringRef arg: args.drop_front()) {
        clang_args.push_back(first_core(arg));
    }

    auto job = query_driver(clang_args);
    if(!job) {
        return job;
    }
    for(std::size_t i = 0; i < report.search_dirs.size(); i += 1) {
        auto& dir = report.search_dirs[i];
        if(report.private_dir == i) {
            job->insert(job->end(), {"-internal-isystem", path::join(resource_dir(), "include")});
            continue;
        }
        job->insert(job->end(),
                    {dir.framework ? "-internal-iframework" : "-internal-isystem", dir.path});
    }
    return job;
}

/// Whether the command names its language standard.
bool names_standard(llvm::ArrayRef<std::string> args) {
    return ranges::any_of(args, [](llvm::StringRef arg) { return arg.starts_with("-std="); });
}

/// cl and clang-cl: clang's own driver in cl mode, which sets the
/// windows-msvc target itself.
std::expected<Job, std::string> ask_cl(llvm::ArrayRef<std::string> args) {
    // Before C++20, clang defers template bodies on MSVC targets so that
    // code written for MSVC's lookup rules compiles, but a deferred body has
    // no AST: every feature inside it goes blind. Placed before the
    // command's own flags, this one keeps that default out of the cc1
    // command and leaves deferral to a command asking for it
    // (`-fdelayed-template-parsing`, clang-cl's `/Zc:twoPhase-`), which
    // every compile of the file then follows.
    std::vector<std::string> cl_args = {args[0],
                                        "--driver-mode=cl",
                                        "-fno-delayed-template-parsing"};
    cl_args.insert(cl_args.end(), args.begin() + 1, args.end());
    return query_driver(cl_args);
}

/// Every other driver is GCC-style: asked how it compiles the input, it
/// shows its job, and the job tells clang underneath (Apple clang, zig,
/// emcc, a `c++` that is clang) from GCC. Neither job is an empty one: an
/// input the driver only passes on (assembler), or a driver that is
/// neither GCC nor clang (Intel's icc).
kota::task<std::expected<Job, std::string>> ask_gcc_style(std::vector<std::string> args,
                                                          std::size_t driver_tokens,
                                                          std::string cwd) {
    // A universal build (`-arch arm64 -arch x86_64`) compiles once for each
    // architecture, and a driver preprocesses for one only: clice reads the
    // code for the first.
    for(std::size_t i = 0, seen = 0; i + 1 < args.size();) {
        if(args[i] == "-arch") {
            seen += 1;
            if(seen > 1) {
                args.erase(args.begin() + i, args.begin() + i + 2);
                continue;
            }
        }
        i += 1;
    }

    auto report = co_await ask_driver(args, driver_tokens, std::move(cwd));
    if(!report) {
        co_return std::unexpected(std::move(report.error()));
    }
    auto standard =
        names_standard(args) || report->standard.empty() ? "" : "-std=" + report->standard;
    if(!report->clang_job.empty()) {
        auto job = std::move(report->clang_job);
        // As for cl: an MSVC target's default template deferral leaves
        // deferred bodies without an AST, so it stays only when the command
        // asks for it.
        if(!ranges::contains(args, "-fdelayed-template-parsing")) {
            std::erase(job, "-fdelayed-template-parsing");
        }
        if(!standard.empty()) {
            job.push_back(std::move(standard));
        }
        co_return job;
    }
    if(!report->gcc_job) {
        co_return Job{};
    }
    if(!standard.empty()) {
        args.push_back(std::move(standard));
    }
    co_return query_as_gcc(*report, args);
}

/// The host compiler nvcc's dryrun names, as a path. The dryrun spells it
/// the way nvcc resolves it — often a bare name that only exists on nvcc's
/// own augmented PATH: that PATH, then next to the nvcc binary, then ours.
std::expected<std::string, std::string> find_nvcc_host(const NVCCDryrunInfo& info,
                                                       llvm::StringRef nvcc) {
    auto executable = [](llvm::StringRef file) {
        return vfs::exists(file) && llvm::sys::fs::can_execute(file);
    };
    llvm::StringRef host = info.host_compiler;
    if(path::is_absolute(host)) {
        return host.str();
    }
    for(auto& dir: info.search_path) {
        if(auto candidate = path::join(dir, host); executable(candidate)) {
            return candidate;
        }
    }
    if(auto sibling = path::join(path::parent_path(nvcc), host); executable(sibling)) {
        return sibling;
    }
    if(auto program = llvm::sys::findProgramByName(host)) {
        return std::move(*program);
    }
    return std::unexpected(std::format("Cannot find nvcc host compiler: {}", host));
}

/// nvcc: its dryrun names the toolkit root, the host compiler and the
/// macros nvcc injects — none of which clang can derive on its own. The
/// host compiler is then asked as a GCC-style driver is, in the input's
/// language and with the command's sysroot, and clang's own driver builds
/// the job, in CUDA mode for a CUDA input.
kota::task<std::expected<Job, std::string>> ask_nvcc(llvm::ArrayRef<std::string> args,
                                                     llvm::StringRef input,
                                                     llvm::StringRef kind,
                                                     std::string cwd) {
    /// nvcc only offloads CUDA-language inputs; a host-language entry
    /// (`nvcc -c foo.cpp`, or an explicit `-x c++`) compiles in a single
    /// host step. The dryrun probe follows the effective language so it
    /// reports the pipeline the real file gets.
    bool cuda_input = kind == "cuda";
    std::vector<std::string> dryrun_args = {args[0], "--dryrun", "-c", input.str()};
    for(llvm::StringRef arg: args) {
        if(is_nvcc_probe_flag(arg)) {
            dryrun_args.push_back(arg.str());
        }
    }
    auto dryrun = co_await execute(std::move(dryrun_args), cwd);
    if(!dryrun) {
        co_return std::unexpected(std::move(dryrun.error()));
    }
    if(!dryrun->failure.empty()) {
        co_return std::unexpected(std::move(dryrun->failure));
    }
    auto info = parse_nvcc_dryrun(dryrun->err);
    if(!info) {
        co_return std::unexpected(std::move(info.error()));
    }

    auto host = find_nvcc_host(*info, args[0]);
    if(!host) {
        co_return std::unexpected(std::move(host.error()));
    }
    auto host_family = Toolchain::driver_family(*host);
    if(host_family == CompilerFamily::MSVC || host_family == CompilerFamily::ClangCL) {
        co_return std::unexpected(std::format("Unsupported nvcc host compiler: {}", *host));
    }
    /// nvcc hands its host compiler a CUDA input as C++. The host's headers
    /// follow the sysroot the command gives it (`-Xcompiler --sysroot=<dir>`).
    std::vector<std::string> host_probe = {*host, "-x", cuda_input ? "c++" : kind.str()};
    for(llvm::StringRef arg: args.drop_front()) {
        if(arg.starts_with("--sysroot=")) {
            host_probe.push_back(arg.str());
        }
    }
    host_probe.push_back(input.str());
    auto host_report = co_await ask_driver(host_probe, 1, std::move(cwd));
    if(!host_report) {
        co_return std::unexpected(std::move(host_report.error()));
    }
    if(host_report->clang_job.empty() && !host_report->gcc_job) {
        co_return std::unexpected(std::format("Unsupported nvcc host compiler: {}", *host));
    }

    std::vector<std::string> cuda_args = {*host, "-fno-delayed-template-parsing"};
    if(cuda_input) {
        /// Without a root from the dryrun, clang's own CUDA detection (which
        /// knows the distro-specific install locations) is the remaining
        /// chance.
        if(!info->cuda_path.empty()) {
            cuda_args.push_back("--cuda-path=" + info->cuda_path);
        }
        /// A toolkit newer than the linked clang still parses, modulo
        /// missing feature macros; without this the version warning would
        /// land in every file's diagnostics.
        cuda_args.push_back("--no-cuda-version-check");

        /// Default to the device-side view: reading CUDA code means reading
        /// the `__CUDA_ARCH__` world — CUTLASS keeps every arch-specific
        /// path behind it, and the host pass grays them all out.
        /// `--cuda-host-only` in the command (e.g. a config rule append)
        /// flips a file to the host view instead; the last selector wins,
        /// matching the driver's own reading of the forwarded flags.
        std::optional<bool> selected_host;
        for(llvm::StringRef arg: args) {
            /// The structured render spells aliases unaliased:
            /// --cuda-host-only arrives as --offload-host-only.
            if(arg == "--offload-host-only") {
                selected_host = true;
            } else if(arg == "--offload-device-only") {
                selected_host = false;
            }
        }
        if(!selected_host) {
            cuda_args.push_back("--cuda-device-only");
        }
        for(auto& define:
            selected_host.value_or(false) ? info->host_defines : info->device_defines) {
            cuda_args.push_back("-D" + define);
        }

        if(!names_standard(args) && !info->cpp_dialect.empty()) {
            cuda_args.push_back("-std=" + info->cpp_dialect);
        }

        /// An `-arch=` probe token is an explicit selection too: its
        /// resolution replaces it in place below.
        bool selects_arch = ranges::any_of(args, [](llvm::StringRef arg) {
            return arg.starts_with("--offload-arch=") || arg.starts_with("-arch=");
        });
        if(!selects_arch && !info->default_arch.empty()) {
            cuda_args.push_back("--cuda-gpu-arch=" + info->default_arch);
        }
    } else {
        /// The single host step still carries nvcc's identity macros
        /// (__NVCC__, version macros) — but no CUDA mode.
        for(auto& define: info->host_defines) {
            cuda_args.push_back("-D" + define);
        }
    }

    for(llvm::StringRef arg: args.drop_front()) {
        if(is_nvcc_probe_flag(arg)) {
            /// The dryrun's resolution of an `-arch=<special>` token lands
            /// exactly where the token sat: an edit-appended selection
            /// carries `--no-offload-arch=all` right before it, and
            /// inserting the resolution any earlier would put it on the
            /// cleared side.
            if(cuda_input && arg.starts_with("-arch=") && !info->default_arch.empty()) {
                cuda_args.push_back("--cuda-gpu-arch=" + info->default_arch);
            }
            continue;
        }
        cuda_args.push_back(arg.str());
    }

    /// A clang host needs no setup: clang's own driver already defaults to
    /// the running machine's triple.
    co_return host_report->gcc_job ? query_as_gcc(*host_report, cuda_args)
                                   : query_driver(cuda_args);
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

/// The driver as a path. The name used to invoke it matters: `clang++` is
/// often a symlink to `clang`, and the name decides C++ mode. So it is
/// never realpath'd, only found: a bare name on PATH, a relative one in the
/// probe working directory.
std::expected<std::string, std::string> find_driver(llvm::StringRef driver, llvm::StringRef cwd) {
    llvm::SmallString<128> found(driver);
    if(!path::is_absolute(driver)) {
        if(driver.contains('/') || driver.contains('\\')) {
            if(!cwd.empty()) {
                found = cwd;
                path::append(found, driver);
            }
        } else if(auto program = llvm::sys::findProgramByName(driver)) {
            found = *program;
        } else {
            return std::unexpected(std::format("Cannot find driver: {}", driver.str()));
        }
    }
    if(!vfs::exists(found) || !llvm::sys::fs::can_execute(found)) {
        return std::unexpected(std::format("Driver {} not found or not executable", found.str()));
    }
    return found.str().str();
}

/// The job the driver runs for the probe's command, over an empty file of
/// the probe's language.
kota::task<std::expected<Job, std::string>> query_one(const QuerySpec& spec) {
    auto driver = find_driver(spec.argv[0], spec.cwd);
    if(!driver) {
        co_return std::unexpected(std::move(driver.error()));
    }

    /// A file of the kind's suffix stands in for the real input, which may
    /// not exist on disk (and a borrowed header must probe as the host's
    /// language, not as its own extension).
    auto input = vfs::temp_file("query-toolchain", temp_suffix_for(spec.kind));
    if(!input) {
        co_return std::unexpected(
            std::format("Failed to create temp file: {}", input.error().message()));
    }
    auto cleanup = llvm::make_scope_exit([&] {
        if(auto e = vfs::remove(*input)) {
            LOG_ERROR("Fail to remove temporary file: {}", e);
        }
    });

    /// The input sits at the slot recorded from the structured command:
    /// selectors after the slot must not govern it (`clang foo.c -x c++`
    /// compiles foo.c as C).
    std::vector<std::string> args(spec.argv.begin(), spec.argv.end());
    args[0] = std::move(*driver);
    args.insert(args.begin() + spec.slot, *input);

    std::expected<Job, std::string> job;
    switch(spec.family) {
        case CompilerFamily::MSVC:
        case CompilerFamily::ClangCL: job = ask_cl(args); break;
        case CompilerFamily::NVCC:
            job = co_await ask_nvcc(args, *input, spec.kind, spec.cwd);
            break;
        default:
            job = co_await ask_gcc_style(std::move(args),
                                         spec.family == CompilerFamily::Zig ? 2 : 1,
                                         spec.cwd);
            break;
    }
    if(job && job->empty()) {
        co_return std::unexpected(std::format("No cc1 args produced for kind {}", spec.kind));
    }
    co_return job;
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

/// clice's own counterpart of a path another clang's job or the command
/// names, if it has one: the job's resource directory (`old_resource_dir`)
/// and its builtin headers, or another clang's builtin header directory a
/// search option names (the command's `-isystem <clang>/lib/clang/23/include`,
/// zig's lib/include). The rest of a resource tree stays external —
/// sanitizer ignorelists under share/, runtime libraries under lib/: the
/// driver verified those files exist there, clice's tree ships include/
/// alone, and cc1 aborts the process on an ignorelist it cannot open.
std::optional<std::string> ours_for(unsigned id,
                                    llvm::StringRef value,
                                    llvm::StringRef old_resource_dir) {
    llvm::StringRef rest = value;
    if(!old_resource_dir.empty() && old_resource_dir != resource_dir() &&
       rest.consume_front(old_resource_dir) &&
       (rest.empty() || rest.front() == '/' || rest.front() == '\\')) {
        rest = rest.ltrim("/\\");
        if(rest.empty()) {
            return resource_dir().str();
        }
        if(rest == "include" || rest.starts_with("include/") || rest.starts_with("include\\")) {
            return path::join(resource_dir(), rest);
        }
        return std::nullopt;
    }
    bool names_search_dir = id == option::OPT_I || id == option::OPT_isystem ||
                            id == option::OPT_idirafter || id == option::OPT_internal_isystem ||
                            id == option::OPT_internal_externc_isystem;
    auto builtin_headers = path::join(resource_dir(), "include");
    if(names_search_dir && value.rtrim("/\\") != builtin_headers &&
       vfs::exists(path::join(value, "__stddef_max_align_t.h"))) {
        return builtin_headers;
    }
    return std::nullopt;
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

Toolchain::Frontend Toolchain::frontend(llvm::ArrayRef<std::string> job) {
    std::vector<std::string> tokens(job.begin() + 2, job.end());
    // An external clang may be newer than the linked one: a flag our cc1
    // does not know parses as unknown and goes with the values it takes
    // (greedy_unknown), instead of those being misread as inputs.
    auto parse_options =
        kota::option::ParseOptions{.greedy_unknown = true, .visibility = option::CC1Option};
    std::vector<kota::option::ParsedArg> kept;
    llvm::StringRef triple;
    llvm::StringRef old_resource_dir;
    for(auto& parsed: option::table().parse(tokens, parse_options)) {
        if(!parsed || parsed->id == option::OPT_UNKNOWN) {
            continue;
        }
        switch(parsed->id) {
            // The probe's own action, input and outputs; the render gives
            // the real file its -main-file-name, and clice manages module
            // outputs itself (the driver derives -fmodule-output=<probe>.pcm
            // for a module unit).
            case option::OPT_E:
            case option::OPT_fsyntax_only:
            case option::OPT_dM:
            case option::OPT_v:
            case option::OPT_o:
            case option::OPT_INPUT:
            case option::OPT_main_file_name:
            case option::OPT_fmodule_output:
            case option::OPT_fmodule_output_EQ: continue;
            case option::OPT_triple: triple = parsed->values[0]; break;
            // A trailing separator on the command's -resource-dir reaches
            // cc1 verbatim while the derived paths append without doubling
            // it.
            case option::OPT_resource_dir:
                old_resource_dir = llvm::StringRef(parsed->values[0]).rtrim("/\\");
                break;
        }
        // A newer external clang may give a known codegen option a value
        // the linked one cannot parse (-mframe-pointer=non-leaf-no-reserve);
        // the parse never needs them.
        if(is_codegen_option(parsed->id)) {
            continue;
        }
        kept.push_back(std::move(*parsed));
    }

    // CPU and ABI names clang's target does not know fail the whole
    // compile: GCC accepts names of its own (`-mtune=intel`, `-mabi=lp64`
    // on Darwin), and a newer external clang knows newer CPUs. The target's
    // defaults stand in.
    clang::DiagnosticOptions diagnostic_options;
    clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(),
                                    diagnostic_options,
                                    new clang::IgnoringDiagConsumer());
    llvm::IntrusiveRefCntPtr<clang::TargetInfo> target;
    if(!triple.empty()) {
        clang::TargetOptions target_options;
        target_options.Triple = triple.str();
        target = clang::TargetInfo::CreateTargetInfo(engine, target_options);
    }
    auto known_name = [&](const kota::option::ParsedArg& arg) {
        llvm::StringRef name = arg.values[0];
        switch(arg.id) {
            case option::OPT_target_cpu: return target->isValidCPUName(name);
            case option::OPT_tune_cpu: return target->isValidTuneCPUName(name);
            case option::OPT_target_abi: return target->setABI(name.str());
            default: return true;
        }
    };

    Frontend result{.driver = db.strings.save(job[0]).data()};
    // A matched LLVM-MinGW installation keeps its own resource tree.
    result.own_resource_dir =
        llvm::Triple(triple).isWindowsGNUEnvironment() && vfs::is_directory(old_resource_dir);
    std::vector<LocalArg> staged;
    for(auto& arg: kept) {
        if(target && !known_name(arg)) {
            continue;
        }
        auto local = db.local_arg(arg, tokens);
        if(!result.own_resource_dir) {
            for(auto& value: local.values) {
                if(auto ours = ours_for(local.opt_id, value, old_resource_dir)) {
                    value = db.strings.save(*ours).data();
                }
            }
        }
        staged.push_back(std::move(local));
    }
    staged.push_back({.opt_id = option::OPT_fsyntax_only, .cls = ArgClass::Semantic});
    result.args = db.persist(staged);
    return result;
}

Toolchain::ResolvedID Toolchain::synthesize(ConfigID id, const Frontend& probe) {
    auto& config = db.config(id);

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

    auto local = [](const Arg& arg) {
        return LocalArg{
            .opt_id = arg.opt_id,
            .cls = arg.cls,
            .spelling = arg.spelling,
            .values = {arg.values.begin(), arg.values.end()}
        };
    };
    std::vector<LocalArg> staged;
    for(auto& arg: probe.args) {
        if(!(full_bmi && arg.opt_id == option::OPT_fmodules_reduced_bmi)) {
            staged.push_back(local(arg));
        }
    }
    /// The config's own user content never fed the probe.
    for(auto& arg: config.args) {
        if(arg.cls != ArgClass::UserContent) {
            continue;
        }
        auto& staged_arg = staged.emplace_back(local(arg));
        if(!probe.own_resource_dir) {
            for(auto& value: staged_arg.values) {
                if(auto ours = ours_for(arg.opt_id, value, {})) {
                    value = db.strings.save(*ours).data();
                }
            }
        }
    }

    resolved_configs.push_back({.driver = probe.driver, .args = db.persist(staged)});
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
                             const std::expected<std::vector<std::string>, std::string>& job) {
    if(!job) {
        failed.try_emplace(key, std::pair{job.error(), std::chrono::steady_clock::now()});
        return;
    }
    probes.try_emplace(key, frontend(*job));
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
