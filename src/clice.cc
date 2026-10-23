module;

#include "version.h"
#include "support/prelude.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

module clice;

import :driver.driver;

extern "C++" int main(int argc, const char** argv) {
#ifdef _WIN32
    // A process without a window is background work to Windows: it is
    // throttled (EcoQoS) onto the efficiency cores of a hybrid CPU, which
    // made every request about twice as slow as on Linux.
    PROCESS_POWER_THROTTLING_STATE throttling{
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0,
    };
    ::SetProcessInformation(::GetCurrentProcess(),
                            ProcessPowerThrottling,
                            &throttling,
                            sizeof(throttling));

    // The CRT's argv is in the ANSI code page, which holds few characters
    // past ASCII: the arguments are read from the UTF-16 command line, by
    // the CRT's rules, as LLVM's tools read them. Its conversion is lenient
    // as theirs: an unpaired surrogate, which a Windows name may hold,
    // becomes U+FFFD instead of emptying the command line.
    const wchar_t* wide = ::GetCommandLineW();
    std::string line(::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr),
                     '\0');
    ::WideCharToMultiByte(CP_UTF8,
                          0,
                          wide,
                          -1,
                          line.data(),
                          static_cast<int>(line.size()),
                          nullptr,
                          nullptr);
    line.pop_back();
    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    llvm::SmallVector<const char*, 16> arguments;
    llvm::cl::TokenizeWindowsCommandLineFull(line, saver, arguments);
    argc = static_cast<int>(arguments.size());
    argv = arguments.data();
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    namespace deco = kota::deco;
    namespace driver = clice::driver;

    auto args = deco::util::argvify(argc, argv);

    deco::cli::SubCommander clice("clice <command> [<args>]",
                                  "A C++ development toolkit built on LLVM/Clang");

    driver::add_commands(clice, argv[0]);

    clice.enable_help().when_err(driver::subcommand_error_handler(clice));

    if(!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        driver::println("clice version {}", clice::version);
        return 0;
    }
    if(!args.empty() && args[0] == "worker") {
        return driver::run_worker(std::span(args).subspan(1));
    }

    return driver::run_subcommands(clice, args);
}
