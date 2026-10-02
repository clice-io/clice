#include <csignal>
#include <print>

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

    int exit_code = 1;

    deco::cli::SubCommander clice("clice <command> [<args>]",
                                  "A C++ development toolkit built on LLVM/Clang");

    auto print_root_usage = [&] {
        std::println("usage: clice <command> [<args>]\n");
        driver::print_usage(clice);
    };

    driver::add_serve(clice, exit_code, self_path);
    driver::add_query(clice, exit_code, self_path);
    driver::add_worker(clice, exit_code);
    driver::add_index(clice, exit_code, self_path);
    driver::add_doc(clice, exit_code);
    driver::add_lint(clice, exit_code, self_path);
    driver::add_format(clice, exit_code);
    driver::add_inspect(clice, exit_code);
    driver::add_analyze(clice, exit_code);

    clice.when_err([&](auto err) {
        if(err.type == deco::cli::SubCommandError::Type::MissingSubCommand) {
            print_root_usage();
            exit_code = 0;
        } else {
            LOG_ERROR("{}", err.message);
        }
    });

    if(!args.empty() && (args[0] == "--version" || args[0] == "-v")) {
        std::println("clice version {}", clice::version);
        return 0;
    }

    if(!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        print_root_usage();
        return 0;
    }

    clice(args);
    return exit_code;
}
