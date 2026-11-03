module;

#include "version.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

module clice;

import :driver.driver;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Clice, description = "A C++ development toolkit built on LLVM/Clang");
struct Clice {
    KANNOTATE(&version,
              cli::names = {"-v", "--version"},
              cli::stop = true,
              description = "Print the version and exit")
    bool version = false;

    KANNOTATE(&command, presence = required)
    cli::Commands<Subcommands> command;
};

}  // namespace

}  // namespace clice::driver

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
    // past ASCII: the arguments are read from the UTF-16 command line.
    std::vector<const char*> arguments;
    for(auto& argument: clice::sys::arguments()) {
        arguments.push_back(argument.c_str());
    }
    argc = static_cast<int>(arguments.size());
    argv = arguments.data();
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    namespace cli = kotatsu::cli;
    namespace driver = clice::driver;

    auto options = cli::parse<driver::Clice>(std::span(argv, argc).subspan(1));
    if(auto help = options.help()) {
        cli::print_usage<driver::Clice>("clice", help->subcommands);
        return 0;
    }
    if(auto error = options.error()) {
        // A command without its operation, `clice query`, is answered with
        // what it takes.
        if(error->kind == cli::Error::Kind::MissingSubcommand) {
            cli::print_usage<driver::Clice>("clice", error->subcommands);
            return 2;
        }
        return driver::usage_error(*error);
    }
    if(options->version) {
        driver::println("clice version {}", clice::version);
        return 0;
    }
    return options->command();
}
