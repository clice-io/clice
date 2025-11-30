#include "Compiler/Toolchain.h"

#include "Compiler/Command.h"
#include "Support/FileSystem.h"
#include "Support/Logging.h"

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

static llvm::ArrayRef<llvm::StringRef> envs() {
    static std::vector<std::string> storage;
    static auto refs = [] {
        std::vector<llvm::StringRef> refs;
        if(environ) {
            for(char** env = environ; *env != nullptr; ++env) {
                llvm::StringRef s(*env);
                if(!s.starts_with("LANG=")) {
                    storage.emplace_back(*env);
                }
            }

            storage.emplace_back("LANG=C");
        }

        /// Note that store the reference os strings in the vector
        /// is not safe when vector grows capacity. But we store it
        /// after all insertion are completed. It's safe here.
        for(const auto& s: storage) {
            refs.emplace_back(s);
        }

        return refs;
    }();
    return refs;
}

#endif

#ifdef _WIN32
llvm::StringRef null_dev = "NUL";
#else
llvm::StringRef null_dev = "/dev/null";
#endif

namespace clice::toolchain {

namespace {

std::optional<std::string> execute_command(llvm::ArrayRef<const char*> arguments,
                                           bool capture_stdout = false) {
    LOG_INFO("Execute command: {}", print_argv(arguments));

    llvm::SmallString<64> path;
    if(auto e = fs::createTemporaryFile("query-toolchain", "clice", path)) {
        LOG_ERROR_RET(std::nullopt, "Fail to create temporary file: {}", e);
    }

    auto _ = llvm::make_scope_exit([&path]() {
        if(auto e = fs::remove(path)) {
            LOG_ERROR("Fail to remove temporary file: {}", e);
        }
    });

#ifdef _WIN32
    /// If the env is `std::nullopt`, `ExecuteAndWait` will inherit env from parent process,
    /// which is very important for msvc and clang on windows. Thay depend on the environment
    /// variables to find correct standard library path.
    constexpr auto env = std::nullopt;
#else
    /// For linux, we should append or modify the "LANG=C" to the env, this is important
    /// for gcc with locality. Otherwise, it will output non-ASCII char. We also want
    /// to inherit the environment variables like windows.
    auto env = envs();
#endif

    std::optional<llvm::StringRef> redirects[3] = {
        {null_dev},                                // stdin
        {capture_stdout ? path.str() : null_dev},  // stdout
        {capture_stdout ? null_dev : path.str()},  // stderr
    };

    llvm::SmallVector<llvm::StringRef> argv(arguments.begin(), arguments.end());

    std::string message;
    if(int rc = llvm::sys::ExecuteAndWait(arguments[0],
                                          argv,
                                          env,
                                          redirects,
                                          /*SecondsToWait=*/0,
                                          /*MemoryLimit=*/0,
                                          &message)) {
        /// FIXME: handle error when rc is positive.
        LOG_ERROR_RET(std::nullopt,
                      "Fail to execute {}, return code is {}, because: {}",
                      arguments[0],
                      rc,
                      message);
    }

    auto file = llvm::MemoryBuffer::getFile(path);
    if(!file) {
        LOG_ERROR_RET(std::nullopt, "Fail to read redirect file: {}", file.getError());
    }

    return file->get()->getBuffer().str();
}

bool query_driver(
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
    /// --driver-mode is not found in the arguments.  and `TargetTriple` is used when
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
        LOG_ERROR_RET(false, "Fail to query driver");
    }

    // We expect to get back exactly one command job, if we didn't something
    // failed. Offload compilation is an exception as it creates multiple jobs. If
    // that's the case, we proceed with the first job. If caller needs a
    // particular job, it should be controlled via options (e.g.
    // --cuda-{host|device}-only for CUDA) passed to the driver.
    const clang::driver::JobList& jobs = compilation->getJobs();
    bool offload_compilation = false;
    if(jobs.size() > 1) {
        for(auto& action: compilation->getActions()) {
            // On MacOSX real actions may end up being wrapped in BindArchAction
            if(llvm::isa<clang::driver::BindArchAction>(action)) {
                action = *action->input_begin();
            }

            if(llvm::isa<clang::driver::OffloadAction>(action)) {
                offload_compilation = true;
                break;
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

struct QueryResult {
    llvm::StringRef target;
    std::vector<llvm::StringRef> includes;
};

/// TODO: use this to print the output of -v.
void parse_version_result(llvm::StringRef content, QueryResult& info) {
    const char* TS = "Target: ";
    const char* SIS = "#include <...> search starts here:";
    const char* SIE = "End of search list.";

    llvm::SmallVector<llvm::StringRef> lines;
    content.split(lines, '\n', -1, false);

    bool in_includes_block = false;
    bool found_start_marker = false;

    for(const auto& line_ref: lines) {
        auto line = line_ref.trim();

        if(line.starts_with(TS)) {
            line.consume_front(TS);
            info.target = line;
            continue;
        }

        if(line == SIS) {
            found_start_marker = true;
            in_includes_block = true;
            continue;
        }

        if(line == SIE) {
            if(in_includes_block) {
                in_includes_block = false;
            }
            continue;
        }

        if(in_includes_block) {
            info.includes.emplace_back(line);
        }
    }

    if(!found_start_marker) {
        LOG_ERROR("Failed to parse version output: missing include search start marker");
        return;
    }

    if(in_includes_block) {
        LOG_ERROR("Failed to parse version output: unclosed include search block");
        return;
    }
}

}  // namespace

CompilerFamily driver_family(llvm::StringRef driver) {
    auto try_get = [](llvm::StringRef name) {
        if(name == "cl") {
            return CompilerFamily::MSVC;
        } else if(name == "nvcc") {
            return CompilerFamily::NVCC;
        } else if(name.ends_with("clang") || name.ends_with("clang++")) {
            return CompilerFamily::Clang;
        } else if(name.ends_with("clang-cl")) {
            return CompilerFamily::ClangCL;
        } else if(name.ends_with("cc") || name.ends_with("c++") || name.ends_with("gcc") ||
                  name.ends_with("g++")) {
            return CompilerFamily::GCC;
        } else if(name.contains("icpc") || name.contains("icc") || name.contains("dpcpp") ||
                  name.contains("icx")) {
            return CompilerFamily::Intel;
        } else if(name.ends_with("zig")) {
            return CompilerFamily::Zig;
        }
        return CompilerFamily::Unknown;
    };

    auto driver_name = llvm::sys::path::filename(driver);
    auto family = try_get(driver_name);
    if(family != CompilerFamily::Unknown) {
        return family;
    }

    // Stripping the executable suffix: clang++.exe -> clang++
    driver_name.consume_back(".exe");
    family = try_get(driver_name);
    if(family != CompilerFamily::Unknown) {
        return family;
    }

    // Stripping any trailing version number: clang++3.5 -> clang++
    driver_name = driver_name.rtrim("0123456789.-");
    family = try_get(driver_name);
    if(family != CompilerFamily::Unknown) {
        return family;
    }

    /// Stripping trailing -component. clang++-tot -> clang++
    driver_name = driver_name.slice(0, driver_name.rfind('-'));
    family = try_get(driver_name);
    return family;
}

std::vector<const char*> query_toolchain(const QueryParams& params) {
    auto arguments = params.arguments;
    llvm::StringRef driver = arguments[0];

    /// Note: The name used to invoke the compiler driver affects its behavior.
    /// For example, `/usr/bin/clang++` is often a symbolic link to
    /// `/usr/lib/llvm-20/bin/clang`. Invoking it as `clang++` enables C++ mode
    /// and links C++ libraries by default, while invoking as `clang` defaults to C mode.
    /// Therefore, never use `realpath` on the initial `driver` name, as that
    /// would lose the context needed for the driver to behave correctly (and break caching).
    llvm::SmallString<128> path;
    if(!path::is_absolute(driver)) {
        /// If the path is not absolute path like g++, find it in the env vars.
        auto program = llvm::sys::findProgramByName(driver);
        if(!program) {
            LOG_ERROR_RET({}, "Fail to query driver, cannot find the driver: {}", driver);
        }
        path = *program;
        driver = path.c_str();
    }

    if(!fs::exists(driver) || !fs::can_execute(driver)) {
        LOG_ERROR_RET({}, "Fail to query driver, driver: {} is not existent or executable", driver);
    }

    auto params_copy = params;
    llvm::SmallVector<const char*, 256> modified_arguments;

    /// Remove driver
    arguments.consume_front();
    modified_arguments.emplace_back(driver.data());

    /// Remove input file
    auto ext = path::extension(params.file);
    ext.consume_front(".");

    modified_arguments.append(arguments.begin(), arguments.end());

    /// Create a file with same suffix of input file, because the input file may
    /// not exist in the disk.
    llvm::SmallString<64> src_path;
    if(auto e = fs::createTemporaryFile("query-toolchain", ext, src_path)) {
        LOG_ERROR_RET({}, "Fail to create temporary file: {}", e);
    }
    auto _ = llvm::make_scope_exit([&src_path]() {
        if(auto e = fs::remove(src_path)) {
            LOG_ERROR("Fail to remove temporary file: {}", e);
        }
    });
    modified_arguments.emplace_back(src_path.c_str());
    arguments = modified_arguments;
    params_copy.arguments = arguments;

    auto family = driver_family(driver);
    switch(family) {
        case CompilerFamily::GCC: {
            return query_gcc_toolchain(params_copy);
        }

        case CompilerFamily::Clang:
        case CompilerFamily::Zig: {
            return query_clang_toolchain(params_copy);
        }
        case CompilerFamily::MSVC:
        case CompilerFamily::ClangCL: {
            return query_msvc_toolchain(params_copy);
        }

        case CompilerFamily::NVCC:
        case CompilerFamily::Intel:
        case CompilerFamily::Unknown: {
            /// TODO: nvcc and intel compilers need further exploration.
            LOG_ERROR("Fail to query driver, unknown supported driver kind: {}, driver is {}",
                      refl::enum_name(family),
                      driver);

            std::vector<const char*> result;
            query_driver(params_copy.arguments,
                         [&](const char* driver, llvm::ArrayRef<const char*> cc1_args) {
                             result.emplace_back(params.callback(driver));
                             for(auto arg: cc1_args) {
                                 result.emplace_back(params.callback(arg));
                             }
                         });
            return result;
        }
    }
}

std::vector<const char*> query_gcc_toolchain(const QueryParams& params) {
    auto arguments = params.arguments;
    llvm::SmallVector<const char*, 256> query_arguments;

    llvm::SmallString<64> target;
    llvm::SmallString<64> install_path;

    query_arguments = {arguments[0], "-dumpmachine"};
    if(auto content = execute_command(query_arguments, true)) {
        target = llvm::StringRef(*content).trim();
    }

    query_arguments = {arguments[0], "-print-search-dirs"};
    if(auto content = execute_command(query_arguments, true)) {
        llvm::SmallVector<llvm::StringRef, 5> lines;
        llvm::StringRef(*content).split(lines, '\n', -1, /*KeepEmpty=*/false);
        for(auto line: lines) {
            line = line.trim();
            if(line.consume_front_insensitive("install:")) {
                install_path = line.trim();
                break;
            }
        }
    }

    target = std::format("--target={}", target);
    install_path = std::format("--gcc-install-dir={}", install_path);

    query_arguments.clear();
    query_arguments.emplace_back(arguments.consume_front());
    query_arguments.emplace_back(target.c_str());
    query_arguments.emplace_back(install_path.c_str());
    query_arguments.append(arguments.begin(), arguments.end());

    std::vector<const char*> result;
    query_driver(query_arguments, [&](const char* driver, llvm::ArrayRef<const char*> cc1_args) {
        result.emplace_back(params.callback(driver));
        for(auto arg: cc1_args) {
            result.emplace_back(params.callback(arg));
        }
    });
    return result;
}

std::vector<const char*> query_clang_toolchain(const QueryParams& params) {
    auto arguments = params.arguments;
    llvm::SmallVector<const char*, 256> query_arguments;

    if(driver_family(arguments[0]) == CompilerFamily::Zig) {
        /// zig cc or zig c++ consumes two arguments.
        query_arguments.emplace_back(arguments.consume_front());
        query_arguments.emplace_back(arguments.consume_front());
    } else {
        query_arguments.emplace_back(arguments.consume_front());
    }

    query_arguments.emplace_back("-###");
    query_arguments.emplace_back("-fsyntax-only");
    query_arguments.append(arguments.begin(), arguments.end());

    std::vector<const char*> result;
    if(auto content = execute_command(query_arguments, false)) {
        llvm::SmallVector<llvm::StringRef> lines;
        llvm::StringRef(*content).split(lines, '\n', -1, /*KeepEmpty=*/false);

        for(llvm::StringRef line: lines) {
            line = line.trim();

            if(line.empty() || line.front() != '"') {
                continue;
            }

            llvm::SmallVector<const char*, 256> args;
            llvm::BumpPtrAllocator allocator;
            llvm::StringSaver saver(allocator);
            llvm::cl::TokenizeGNUCommandLine(line, saver, args);

            using namespace std::string_view_literals;
            if(args.size() < 2 || args[1] != "-cc1"sv) {
                continue;
            }

            for(auto arg: args) {
                if(arg == "-###"sv) {
                    continue;
                }
                result.emplace_back(params.callback(arg));
            }
        }
    }
    return result;
}

std::vector<const char*> query_msvc_toolchain(const QueryParams& params) {
    auto arguments = params.arguments;
    llvm::SmallVector<const char*, 256> query_arguments;

    query_arguments.emplace_back(arguments.consume_front());
    /// When clang in cl mode, the target will be set to windows-msvc automatically.
    /// We don't need to add extra flag.
    query_arguments.emplace_back("--driver-mode=cl");
    query_arguments.append(arguments.begin(), arguments.end());

    std::vector<const char*> result;
    query_driver(query_arguments, [&](const char* driver, llvm::ArrayRef<const char*> cc) {
        result.emplace_back(params.callback(driver));
        for(auto c: cc) {
            result.emplace_back(params.callback(c));
        }
    });
    return result;
}

std::vector<const char*> query_nvcc_toolchain(const QueryParams& params);

}  // namespace clice::toolchain
