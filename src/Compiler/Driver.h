#pragma once

#include "Compiler/Command.h"
#include "Support/Logging.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "clang/Driver/Driver.h"

namespace clice {

namespace {

namespace opt = llvm::opt;
namespace driver = clang::driver;

/// Checks if dash-dash (`--`) parsing is enabled. If enabled, all arguments
/// after a standalone `--` are treated as positional arguments (e.g., input files).
bool enable_dash_dash_parsing(const opt::OptTable& table);

/// Checks if grouped short options are enabled. If enabled, a short option group
/// like `-ab` is parsed as separate options `-a` and `-b`.
bool enable_grouped_short_options(const opt::OptTable& table);

/// Get the specific toolchain of given target, we mainly use it to get msvc toolchain.
const driver::ToolChain& get_toolchain(driver::Driver& driver,
                                       const opt::ArgList& Args,
                                       const llvm::Triple& Target);

template <auto MP1, auto MP2, auto MP3>
struct Thief {
    friend bool enable_dash_dash_parsing(const opt::OptTable& table) {
        return table.*MP1;
    }

    friend bool enable_grouped_short_options(const opt::OptTable& table) {
        return table.*MP2;
    }

    friend const driver::ToolChain& get_toolchain(driver::Driver& driver,
                                                  const opt::ArgList& args,
                                                  const llvm::Triple& target) {
        return (driver.*MP3)(args, target);
    }
};

template struct Thief<&opt::OptTable::DashDashParsing,
                      &opt::OptTable::GroupedShortOptions,
                      &driver::Driver::getToolChain>;

class ArgumentParser final : public llvm::opt::ArgList {
public:
    ArgumentParser(llvm::BumpPtrAllocator* allocator) : allocator(allocator) {}

    ~ArgumentParser() {
        /// We never use the private `Args` field, so make sure it's empty.
        if(getArgs().size() != 0) {
            std::abort();
        }
    }

    const char* getArgString(unsigned index) const override {
        return arguments[index];
    }

    unsigned getNumInputArgStrings() const override {
        return arguments.size();
    }

    const char* MakeArgStringRef(llvm::StringRef s) const override {
        auto p = allocator->Allocate<char>(s.size() + 1);
        std::ranges::copy(s, p);
        p[s.size()] = '\0';
        return p;
    }

    inline static auto& option_table = clang::driver::getDriverOptTable();

    void set_arguments(llvm::ArrayRef<const char*> arguments) {
        if(getArgs().size() != 0) {
            std::abort();
        }

        this->arguments = arguments;
    }

    std::unique_ptr<llvm::opt::Arg> parse_one(unsigned& index) {
        /// Make sure we are not using
        assert(!enable_dash_dash_parsing(option_table));
        assert(!enable_grouped_short_options(option_table));
        return option_table.ParseOneArg(*this, index);
    }

    void parse(llvm::ArrayRef<const char*> arguments, const auto& on_parse, const auto& on_error) {
        this->arguments = arguments;

        unsigned it = 0;
        while(it != arguments.size()) {
            llvm::StringRef s = arguments[it];

            if(s.empty()) [[unlikely]] {
                it += 1;
                continue;
            }

            auto prev = it;
            auto arg = parse_one(it);
            assert(it > prev && "parser failed to consume argument");

            if(!arg) [[unlikely]] {
                assert(it >= arguments.size() && "unexpected parser error!");
                assert(it - prev - 1 && "no missing arguments!");

                /// FIXME: When parsing fails, the parser may have encountered unknown
                /// arguments (e.g., options for a different compiler like nvcc).
                /// We should allow the user to provide a custom option registry
                /// (mainly for these pass-through arguments).
                ///
                /// This would let us ignore them correctly. For example, when
                /// parsing `nvcc --option-dir x.txt main.cpp`, our parser fails
                /// because it discards `--option-dir` but doesn't know it also
                /// consumes the next argument (`x.txt`).
                ///
                /// With a custom registry, we could register that `--option-dir`
                /// takes one argument, allowing us to skip both and continue
                /// parsing from `main.cpp`.
                on_error(prev, it - prev - 1);
                break;
            }

            on_parse(std::move(arg));
        }
    }

private:
    llvm::BumpPtrAllocator* allocator;

    llvm::ArrayRef<const char*> arguments;
};

using ErrorKind = QueryDriverError::ErrorKind;

auto unexpected(ErrorKind kind, std::string message) {
    return std::unexpected<QueryDriverError>({kind, std::move(message)});
};

enum class CompilerFamily {
    Unknown,
    GCC,      // Covers gcc, g++, cc, c++, and versioned/arch variants
    Clang,    // Covers clang, clang++, and versioned variants (excluding clang-cl)
    MSVC,     // Covers cl
    ClangCL,  // Covers clang-cl explicitly
    NVCC,     // Covers nvcc
    Intel,    // Covers icc, icpc, icx, dpcpp
    Zig,      // Covers zig cc / zig c++ (assumed GCC/Clang compatible for query)
};

CompilerFamily driver_family(llvm::StringRef driver) {
    auto driver_name = llvm::sys::path::filename(driver);
    driver_name.consume_back(".exe");
    if(driver_name == "cl") {
        return CompilerFamily::MSVC;
    } else if(driver_name == "nvcc") {
        return CompilerFamily::NVCC;
    } else if(driver_name.contains("clang-cl")) {
        return CompilerFamily::ClangCL;
    } else if(driver_name.contains("clang")) {
        return CompilerFamily::Clang;
    } else if(driver_name == "cc" || driver_name == "c++" || driver_name.contains("gcc") ||
              driver_name.contains("g++")) {
        return CompilerFamily::GCC;
    } else if(driver_name.contains("icpc") || driver_name.contains("icc") ||
              driver_name.contains("dpcpp") || driver_name.contains("icx")) {
        return CompilerFamily::Intel;
    } else if(driver_name.contains("zig")) {
        return CompilerFamily::Zig;
    }
    return CompilerFamily::Unknown;
}

struct QueryResult {
    std::string target;
    llvm::SmallVector<std::string, 8> includes;
};

auto parse_query_result(llvm::StringRef content, QueryResult& info)
    -> std::expected<void, QueryDriverError> {
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
        return unexpected(ErrorKind::InvalidOutputFormat, "Start marker not found...");
    }

    if(in_includes_block) {
        return unexpected(ErrorKind::InvalidOutputFormat, "End marker not found...");
    }

    return std::expected<void, QueryDriverError>();
}

auto query_driver(llvm::StringRef driver) -> std::expected<QueryResult, QueryDriverError> {
    llvm::SmallString<128> path;

    /// Note: The name used to invoke the compiler driver affects its behavior.
    /// For example, `/usr/bin/clang++` is often a symbolic link to
    /// `/usr/lib/llvm-20/bin/clang`. Invoking it as `clang++` enables C++ mode
    /// and links C++ libraries by default, while invoking as `clang` defaults to C mode.
    /// Therefore, never use `realpath` on the initial `driver` name, as that
    /// would lose the context needed for the driver to behave correctly (and break caching).
    if(!llvm::sys::path::is_absolute(driver)) {
        /// If the path is not absolute path like g++, find it in the env vars.
        auto program = llvm::sys::findProgramByName(driver);
        if(!program) {
            return unexpected(ErrorKind::NotFoundInPATH, program.getError().message());
        }
        path = *program;
        driver = path;
    }

    /// Check whether we can execute the driver.
    if(!llvm::sys::fs::exists(driver) || !llvm::sys::fs::can_execute(driver)) {
        /// FIXME: Add whitelisting, blacklisting (do not trust workspace executables),
        /// and toolchain integrity checks.
        return unexpected(ErrorKind::NotFoundInPATH, "");
    }

    auto family = driver_family(driver);

    /// FIXME: Handle nvcc and intel compiler.
    if(family == CompilerFamily::NVCC || family == CompilerFamily::Intel ||
       family == CompilerFamily::Zig || family == CompilerFamily::Unknown) [[unlikely]] {
        /// FIXME: nvcc and intel compilers need further exploration.
        /// zig is easy to handle, just use `zig cc` or `zig c++`, then
        /// it will behave like clang.
        return unexpected(ErrorKind::NotImplemented, "");
    }

    /// Query the compiler for includes information.
    if(family == CompilerFamily::GCC || family == CompilerFamily::Clang) {
        llvm::SmallString<128> output_path;
        if(auto error =
               llvm::sys::fs::createTemporaryFile("system-includes", "clice", output_path)) {
            return unexpected(ErrorKind::FailToCreateTempFile, error.message());
        }

        // If we fail to get the driver infomation, keep the output file for user to debug.
        bool keep_output_file = true;
        auto clean_up = llvm::make_scope_exit([&output_path, &keep_output_file]() {
            if(keep_output_file) {
                logging::warn("Query driver failed, output file:{}", output_path);
                return;
            }

            if(auto errc = llvm::sys::fs::remove(output_path)) {
                logging::warn("Fail to remove temporary file: {}", errc.message());
            }
        });

        /// FIXME: Is it possible that the output is not in stderr?
        std::optional<llvm::StringRef> redirects[3] = {
            {""},
            {""},
            {output_path.str()},
        };

#ifdef _WIN32
        /// If the env is `std::nullopt`, `ExecuteAndWait` will inherit env from parent process,
        /// which is very important for msvc and clang on windows. Thay depend on the environment
        /// variables to find correct standard library path.
        constexpr auto env = std::nullopt;

        llvm::SmallVector<llvm::StringRef, 6> argv = {driver, "-E", "-v", "-xc++", "NUL"};
#else
        /// FIXME: We should find a better way to convert "LANG=C", this is important
        /// for gcc with locality. Otherwise, it will output non-ASCII char. We also
        /// want to inherit the environment variables like windows.
        llvm::SmallVector<llvm::StringRef> env = {"LANG=C"};
        llvm::SmallVector<llvm::StringRef> argv = {driver, "-E", "-v", "-xc++", "/dev/null"};
#endif

        std::string message;
        if(int RC = llvm::sys::ExecuteAndWait(driver,
                                              argv,
                                              env,
                                              redirects,
                                              /*SecondsToWait=*/0,
                                              /*MemoryLimit=*/0,
                                              &message)) {
            return unexpected(ErrorKind::InvokeDriverFail, std::move(message));
        }

        auto file = llvm::MemoryBuffer::getFile(output_path);
        if(!file) {
            return unexpected(ErrorKind::OutputFileNotReadable, file.getError().message());
        }

        QueryResult info;
        if(auto r = parse_query_result(file.get()->getBuffer(), info)) {
            keep_output_file = false;
            return info;
        } else {
            return std::unexpected(r.error());
        }
    }

    /// For msvc and clang-cl, we don't need to query driver. Just use clang
    /// tool chain to find the built includes.
    if(family == CompilerFamily::MSVC || family == CompilerFamily::ClangCL) {
        /// FIXME: target information? e.g. arm cross compilation.
        llvm::StringRef target = "x86_64-pc-windows-msvc";

        /// An workaround to use clang's toolchain to find vsinstall information
        /// and related includes.
        clang::DiagnosticOptions options;
        thread_local clang::DiagnosticsEngine engine(new clang::DiagnosticIDs(), options);
        clang::driver::Driver driver("", target, engine);
        llvm::SmallVector<const char*> args = {"", "-xc++", "NUL"};
        llvm::opt::InputArgList list(args.begin(), args.end());
        auto& toolchain = get_toolchain(driver, list, llvm::Triple(target));

        /// FIXME: specify specific version of vs?
        llvm::opt::ArgStringList includes;
        toolchain.AddClangSystemIncludeArgs(list, includes);

        QueryResult info;
        info.target = target;
        for(auto& include: includes) {
            info.includes.emplace_back(include);
        }
        return info;
    }

    std::unreachable();
}

}  // namespace

}  // namespace clice
