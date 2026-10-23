module;

#include "support/logging.macros.h"
#include "support/prelude.h"

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
        job = cl ? ask_clang(args, {"--driver-mode=cl"}, spec.cwd)
                 : ask_clang_for(std::move(args), driver_tokens, spec.cwd);
    } else if(cl) {
        job = co_await ask_cl(args, spec.family, *input, spec.cwd);
    } else if(spec.family == CompilerFamily::NVCC) {
        job = co_await ask_nvcc(args, *input, spec.kind, spec.cwd);
    } else {
        job = co_await ask_gcc_style(std::move(args), driver_tokens, spec.cwd);
    }
    co_return job;
}

}  // namespace clice
