#include "server/options.h"
#include "server/server.h"
#include "support/logging.h"

int main(int argc, const char** argv) {
    clice::logging::stderr_logger("clice", clice::logging::options);

    auto options = clice::Options::parse(argc, argv);
    options.compute_defaults();

    switch(options.mode) {
        case clice::Options::Mode::Pipe:
            return clice::run_pipe_mode(options);
        case clice::Options::Mode::Socket:
            clice::logging::info("Socket mode not yet implemented");
            return 1;
        case clice::Options::Mode::StatelessWorker:
            clice::logging::info("StatelessWorker mode not yet implemented");
            return 1;
        case clice::Options::Mode::StatefulWorker:
            clice::logging::info("StatefulWorker mode not yet implemented");
            return 1;
    }

    return 0;
}
