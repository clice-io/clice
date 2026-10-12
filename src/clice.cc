module;

#include "modules/prelude.h"

#include "version.h"

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
