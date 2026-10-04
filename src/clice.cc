#include <csignal>

#include "version.h"
#include "driver/driver.h"

#include "kota/deco/deco.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, const char** argv) {
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
    const char* self_path = argv[0];

    deco::cli::SubCommander clice("clice <command> [<args>]",
                                  "A C++ development toolkit built on LLVM/Clang");

    driver::add_serve(clice, self_path);
    driver::add_query(clice, self_path);
    driver::add_refactor(clice, self_path);
    driver::add_worker(clice);
    driver::add_index(clice, self_path);
    driver::add_lint(clice, self_path);
    driver::add_format(clice);
    driver::add_inspect(clice);
    driver::add_analyze(clice);

    clice.enable_help().when_err(driver::subcommand_error_handler(clice));

    if(!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        driver::println("clice version {}", clice::version);
        return 0;
    }

    return clice(args);
}
