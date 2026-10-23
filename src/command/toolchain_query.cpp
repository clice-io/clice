module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :command.argument;
import :command.toolchain_query;
import :command.toolchains.cl;
import :command.toolchains.clang;
import :command.toolchains.gcc;
import :command.toolchains.nvcc;
import :support.logging;
import :vfs.cygwin;
import :vfs.file_system;
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
/// query working directory. Only a Cygwin symlink is followed, which
/// Windows cannot start.
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
#ifdef _WIN32
    // Cygwin and MSYS2 tools, and a Cygwin symlink's target, name programs
    // without the `.exe` Windows adds when it starts one (`gcc-12.2.0`).
    auto with_exe = [](llvm::SmallString<128>& program) {
        if(!vfs::exists(program) && !program.str().ends_with_insensitive(".exe") &&
           vfs::exists(program.str().str() + ".exe")) {
            program += ".exe";
        }
    };
    with_exe(found);
    // A link may name another (clang++ -> clang -> clang-15), and a chain
    // that loops ends at the bound.
    int links = 0;
    while(auto target = read_cygwin_symlink(found)) {
        links += 1;
        if(is_posix_path(*target) || links > 16) {
            return std::unexpected(
                std::format("Driver {} is a Cygwin symlink to {}: name the program it links to",
                            found.str(),
                            *target));
        }
        found = (path::parent_path(found) + "/" + *target).str();
        with_exe(found);
    }
#endif
    if(!vfs::exists(found) || !llvm::sys::fs::can_execute(found)) {
        return std::unexpected(std::format("Driver {} not found or not executable", found.str()));
    }
    return found.str().str();
}

/// The triple of a cc1 job and the CPU, tuning and ABI names it gives that
/// clang's target for the triple does not know.
std::pair<std::string, llvm::StringSet<>> unknown_names(const Job& job) {
    std::vector<std::string> cc1(job.begin() + 2, job.end());
    auto parse_options = kota::option::ParseOptions{.visibility = option::CC1Option};
    std::string triple;
    std::vector<kota::option::ParsedArg> given;
    for(auto& parsed: option::table().parse(cc1, parse_options)) {
        if(!parsed) {
            continue;
        }
        if(parsed->id == option::OPT_triple) {
            triple = parsed->values[0];
        } else if(!parsed->values.empty()) {
            given.push_back(std::move(*parsed));
        }
    }
    TargetNames names(triple);
    llvm::StringSet<> unknown;
    for(auto& arg: given) {
        if(!names.knows(arg.id, arg.values[0])) {
            unknown.insert(arg.values[0]);
        }
    }
    return {std::move(triple), std::move(unknown)};
}

/// Whether a driver option names one of `unknown` as the CPU, tuning or ABI
/// (`-mcpu=cortex-a72+crc` names cortex-a72).
bool names_unknown(const kota::option::ParsedArg& arg, const llvm::StringSet<>& unknown) {
    switch(arg.id) {
        case option::OPT_mcpu_EQ:
        case option::OPT_march_EQ:
        case option::OPT_mtune_EQ:
        case option::OPT_mabi_EQ:
            return unknown.contains(llvm::StringRef(arg.values[0]).split('+').first);
        default: return false;
    }
}

}  // namespace

kota::task<std::expected<Job, std::string>> query_one(const QuerySpec& spec) {
    auto driver = find_driver(spec.argv[0], spec.cwd);
    if(!driver && spec.query_toolchain) {
        co_return std::unexpected(std::move(driver.error()));
    }

    /// A file of the kind's suffix stands in for the real input, which may
    /// not exist on disk (and a borrowed header must query as the host's
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
    if(driver) {
        args[0] = std::move(*driver);
    }
    args.insert(args.begin() + spec.slot, *input);

    std::size_t driver_tokens = spec.family == CompilerFamily::Zig ? 2 : 1;
    std::expected<Job, std::string> job;
    bool cl = cl_mode(spec.family);
    if(!spec.query_toolchain) {
        job = std::move(read_as_clang(std::move(args), spec.family, spec.cwd).job);
    } else if(cl) {
        job = co_await ask_cl(args, spec.family, *input, spec.cwd);
    } else if(spec.family == CompilerFamily::NVCC) {
        job = co_await ask_nvcc(args, *input, spec.kind, spec.cwd);
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
    auto parse_options = kota::option::ParseOptions{.visibility = family_visibility(family)};
    std::string named;
    std::vector<std::string> flags(args.begin() + 1, args.end());
    for(auto& parsed: option::table().parse(flags, parse_options)) {
        if(parsed && parsed->id == option::OPT_target) {
            named = parsed->values[0];
        }
    }

    std::vector<std::string> mode;
    if(cl_mode(family)) {
        mode.push_back("--driver-mode=cl");
    }
    // clang reads a target off its program's name in its main alone, never
    // when its library builds a compilation.
    auto target = named.empty() ? name_target(args[0]) : named;
    if(named.empty() && !target.empty()) {
        LOG_INFO("clang reads the command of {} for {}, the target its name sets", args[0], target);
        mode.push_back("--target=" + target);
    }
    if(family == CompilerFamily::GCC) {
        llvm::Triple triple(target.empty() ? llvm::sys::getDefaultTargetTriple() : target);
        for(auto& arg: llvm::MutableArrayRef<std::string>(args).drop_front()) {
            arg = for_clang(arg, triple);
        }
    }

    ClangReading reading{.argv = clang_argv(args, mode)};
    reading.job = query_driver(reading.argv, cwd, quiet);
    if(!reading.job) {
        return reading;
    }
    // The options naming what clang's target does not know go, and the
    // driver reads the command again without them.
    auto [triple, unknown] = unknown_names(*reading.job);
    if(unknown.empty()) {
        return reading;
    }
    flags.assign(reading.argv.begin() + 1, reading.argv.end());
    std::vector<std::string> argv = {reading.argv[0]};
    std::size_t next = 0;
    for(auto& parsed: option::table().parse(flags, parse_options)) {
        if(parsed && names_unknown(*parsed, unknown)) {
            LOG_INFO("clang's target {} knows no CPU or ABI {} names: its default stands in",
                     triple,
                     flags[parsed->index]);
            argv.insert(argv.end(), flags.begin() + next, flags.begin() + parsed->index);
            next = parsed->index + 1;
        }
    }
    argv.insert(argv.end(), flags.begin() + next, flags.end());
    reading.argv = std::move(argv);
    reading.job = query_driver(reading.argv, cwd, quiet);
    return reading;
}

}  // namespace clice
