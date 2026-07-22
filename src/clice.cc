#include <csignal>
#include <print>

#include "version.h"
#include "driver/driver.h"

#include "kota/deco/deco.h"

int main(int argc, const char** argv) {
#ifndef _WIN32
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
    driver::add_query(clice, exit_code);
    driver::add_worker(clice, exit_code);
    driver::add_index(clice, exit_code);
    driver::add_doc(clice, exit_code);
    driver::add_lint(clice, exit_code);
    driver::add_format(clice, exit_code);

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
