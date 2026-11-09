module clice;

import :driver.driver;
import :support.logging;
import :support.process;
import :vfs.path;
import :worker.stateful;
import :worker.stateless;

namespace clice::driver {

namespace {

/// What the master spawns its workers as: no command of a user's, so no line
/// of `clice --help`.
struct Worker {
    KANNOTATE(&stateful, description = "Run as stateful worker (default: stateless)")
    bool stateful = false;

    KANNOTATE(&worker_name, description = "Name the worker logs under")
    std::string worker_name = "worker";

    KANNOTATE(&log_dir, description = "Directory of the worker's log file (default: no log file)")
    std::string log_dir;
};

/// Not through dispatch: a worker logs as the pool relays its stderr.
int run_worker(Worker& worker) {
    // Started through /proc/self/exe on Linux, which `top` shows as `exe`:
    // named after the program and the worker (`clice SF-0`), cut at 15
    // bytes; elsewhere the name stays the program's. Threads take the name
    // they start with.
    if(auto& arguments = sys::arguments(); !arguments.empty()) {
        sys::set_process_name(
            std::format("{} {}", path::filename(arguments.front()), worker.worker_name));
    }
    if(!worker.stateful) {
        // Limit libuv thread pool to 1 thread so each stateless worker
        // executes only one compilation at a time. Must be set before any
        // other thread starts and before any async::queue call.
        async::set_thread_pool_size(1);
    }
    // The pool relays each line of a worker's stderr as a warning: one
    // with no log file to write keeps its lower levels out of it.
    logging::options.stderr_level = logging::Level::warn;
    logging::options.stderr_crash_trace = false;
    logging::stderr_logger(worker.worker_name);
    if(!worker.log_dir.empty()) {
        // File only: worker stderr is reserved for crash/unexpected output,
        // which the master relays into its own log (see logging taxonomy).
        logging::file_logger(worker.worker_name, worker.log_dir, /*mirror_stderr=*/false);
    }
    // See WorkerPool::launch.
    if(auto error = sys::exit_with_parent()) {
        LOG_ERROR("Cannot watch the master, exiting: {}", error.message());
        return 1;
    }
    return worker.stateful ? run_stateful_worker_mode() : run_stateless_worker_mode();
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(run_worker, {.hidden = true});

}  // namespace

}  // namespace clice::driver
