module clice;

import :command.argument;
import :command.toolchain_query;
import :command.toolchains.cl;
import :command.toolchains.clang;
import :command.toolchains.gcc;
import :command.toolchains.nvcc;
import :support.logging;
import :vfs.path;

namespace clice {

namespace {

/// The temp-file extension a query input uses for `kind`: clang's own
/// suffix when the kind is a known language, the raw extension itself
/// otherwise (the driver ends up exactly as confused as it would be by the
/// real file — the query fails the same way the real compile would).
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

/// The driver as a path. The name used to invoke it matters: `clang++` is
/// often a symlink to `clang`, and the name decides C++ mode. So it is
/// never realpath'd, only found: a bare name on PATH, a relative one in the
/// query working directory. Only a Cygwin symlink, which Windows cannot
/// start, is followed to the program it names.
std::expected<std::string, std::string> find_driver(llvm::StringRef driver, llvm::StringRef cwd) {
    bool bare = !driver.contains('/') && !driver.contains('\\');
    auto name = bare || cwd.empty() ? driver.str() : path::join(cwd, driver);
    // Cygwin and MSYS2 programs, and a Cygwin symlink's target, are named
    // without the `.exe` Windows adds when it starts one (`gcc-12.2.0`); a
    // driver may be a batch file (emscripten's `emcc.bat`).
    sys::ProgramSearch search{
        .extensions = {"", ".exe", ".bat", ".cmd"}
    };
    std::string found;
    if(auto error = sys::find_program(found, name, search)) {
        std::string target;
        if(error == std::errc::permission_denied && fs::cygwin::read_symlink(target, found)) {
            return std::unexpected(
                std::format("Driver {} is a Cygwin symlink to {}: name the program it links to",
                            found,
                            target));
        }
        if(bare) {
            return std::unexpected(std::format("Cannot find driver: {}", driver.str()));
        }
        return std::unexpected(std::format("Driver {} not found or not executable", name));
    }
    return found;
}

/// The names a cc1 job gives that clang's target does not know, each with
/// its option (-target-cpu, -tune-cpu, -target-abi), and the job's triple.
struct UnknownNames {
    std::string triple;

    std::set<std::pair<unsigned, std::string>> names;
};

UnknownNames unknown_names(const Job& job) {
    std::vector<std::string> cc1(job.args.begin() + 2, job.args.end());
    auto parse_options = option::ParseOptions{.visibility = clang_options::CC1Option};
    UnknownNames result;
    std::vector<option::ParsedArg> given;
    for(auto& parsed: clang_options::table().parse(cc1, parse_options)) {
        if(!parsed) {
            continue;
        }
        if(parsed->id == clang_options::OPT_triple) {
            result.triple = parsed->values[0];
        } else if(!parsed->values.empty()) {
            given.push_back(std::move(*parsed));
        }
    }
    TargetNames names(result.triple);
    for(auto& arg: given) {
        if(!names.knows(arg.id, arg.values[0])) {
            result.names.emplace(arg.id, arg.values[0]);
        }
    }
    return result;
}

/// Whether a driver option names one of `unknown`: the CPU of -mcpu,
/// -march and AVR's -mmcu (`-mcpu=cortex-a72+crc` names cortex-a72,
/// `native` this machine's), the tuning CPU of -mtune, the ABI of -mabi.
bool names_unknown(const option::ParsedArg& arg,
                   const std::set<std::pair<unsigned, std::string>>& unknown) {
    unsigned given = 0;
    switch(arg.id) {
        case clang_options::OPT_mcpu_EQ:
        case clang_options::OPT_march_EQ:
        case clang_options::OPT_mmcu_EQ: given = clang_options::OPT_target_cpu; break;
        case clang_options::OPT_mtune_EQ: given = clang_options::OPT_tune_cpu; break;
        case clang_options::OPT_mabi_EQ: given = clang_options::OPT_target_abi; break;
        default: return false;
    }
    auto name = llvm::StringRef(arg.values[0]).split('+').first;
    if(name == "native") {
        name = llvm::sys::getHostCPUName();
    }
    return unknown.contains({given, name.str()});
}

}  // namespace

async::task<std::expected<Job, std::string>> query_one(const QuerySpec& spec) {
    auto driver = find_driver(spec.argv[0], spec.cwd);
    if(!driver && spec.query_toolchain) {
        co_return std::unexpected(std::move(driver.error()));
    }

    /// A file of the kind's suffix stands in for the real input, which may
    /// not exist on disk (and a borrowed header must query as the host's
    /// language, not as its own extension).
    std::string dir;
    sys::temp_dir(dir);
    auto input = fs::TempFile::create(dir, "query-toolchain-", "." + temp_suffix_for(spec.kind));
    if(!input) {
        co_return std::unexpected(
            std::format("Failed to create temp file: {}", input.error().message()));
    }
    // A Windows compiler may open its input without sharing it for writing.
    input->file().close();

    /// The input sits at the slot recorded from the structured command:
    /// selectors after the slot must not govern it (`clang foo.c -x c++`
    /// compiles foo.c as C).
    std::vector<std::string> args(spec.argv.begin(), spec.argv.end());
    if(driver) {
        args[0] = std::move(*driver);
    }
    args.insert(args.begin() + spec.slot, std::string(input->path()));

    std::size_t driver_tokens = spec.family == CompilerFamily::Zig ? 2 : 1;
    std::expected<Job, std::string> job;
    bool cl = cl_mode(spec.family);
    if(!spec.query_toolchain) {
        job = std::move(read_as_clang(std::move(args), spec.family, spec.cwd).job);
    } else if(cl) {
        job = co_await ask_cl(args, spec.family, input->path(), spec.cwd);
    } else if(spec.family == CompilerFamily::NVCC) {
        job = co_await ask_nvcc(args, input->path(), spec.kind, spec.cwd);
    } else {
        job = co_await ask_gcc_style(std::move(args), driver_tokens, spec.cwd);
    }
    co_return job;
}

ClangReading read_as_clang(std::vector<std::string> args,
                           CompilerFamily family,
                           llvm::StringRef cwd,
                           bool quiet) {
    if(family == CompilerFamily::Zig) {
        args.erase(args.begin() + 1);
    }
    auto parse_options = option::ParseOptions{.visibility = family_visibility(family)};
    std::string named;
    std::vector<std::string> flags(args.begin() + 1, args.end());
    for(auto& parsed: clang_options::table().parse(flags, parse_options)) {
        if(parsed && parsed->id == clang_options::OPT_target) {
            named = parsed->values[0];
        }
    }

    std::vector<std::string> mode;
    if(cl_mode(family)) {
        mode = cl_driver_mode(args[0], family);
    }
    // clang reads a target off its program's name in its main alone, never
    // when its library builds a compilation.
    auto target = named.empty() ? name_target(args[0]) : named;
    if(named.empty() && !target.empty()) {
        if(!quiet) {
            LOG_INFO("clang reads the command of {} for {}, the target its name sets",
                     args[0],
                     target);
        }
        mode.push_back("--target=" + target);
    }
    std::vector<std::string> taken = {args[0]};
    auto taken_flags = flags_for_clang(llvm::ArrayRef(args).drop_front(), family);
    taken.insert(taken.end(), taken_flags.begin(), taken_flags.end());
    if(family == CompilerFamily::GCC) {
        llvm::Triple triple(target.empty() ? llvm::sys::getDefaultTargetTriple() : target);
        for(auto& arg: llvm::MutableArrayRef<std::string>(taken).drop_front()) {
            arg = for_clang(arg, triple, quiet);
        }
    }

    ClangReading reading{.argv = clang_argv(taken, mode)};
    std::vector<std::string> errors;
    reading.job = query_driver(reading.argv, cwd, &errors);
    // The options naming what clang's target does not know go, and the
    // driver reads the command again without them; one a later option
    // overrode names it only once that one goes (`-mabi=eabi -mabi=o64`).
    while(reading.job) {
        auto unknown = unknown_names(*reading.job);
        flags.assign(reading.argv.begin() + 1, reading.argv.end());
        std::vector<std::string> argv = {reading.argv[0]};
        std::size_t next = 0;
        bool dropped = false;
        for(auto& parsed: clang_options::table().parse(flags, parse_options)) {
            if(parsed && names_unknown(*parsed, unknown.names)) {
                if(!quiet) {
                    LOG_INFO(
                        "clang's target {} knows no CPU or ABI {} names: its default stands in",
                        unknown.triple,
                        flags[parsed->index]);
                }
                argv.insert(argv.end(), flags.begin() + next, flags.begin() + parsed->index);
                next = parsed->index + 1;
                dropped = true;
            }
        }
        if(!dropped) {
            break;
        }
        argv.insert(argv.end(), flags.begin() + next, flags.end());
        reading.argv = std::move(argv);
        reading.job = query_driver(reading.argv, cwd, &errors);
    }
    if(reading.job && !quiet) {
        for(auto& error: errors) {
            LOG_WARN("clang's driver, reading the command of {}: {}", reading.argv[0], error);
        }
    }
    return reading;
}

}  // namespace clice
