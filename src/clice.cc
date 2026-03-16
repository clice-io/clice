#include "server/options.h"
#include "server/server.h"
#include "server/stateless_worker.h"
#include "server/stateful_worker.h"
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
            return clice::run_stateless_worker_mode(options);
        case clice::Options::Mode::StatefulWorker:
            return clice::run_stateful_worker_mode(options);
    }

    return 0;
}
