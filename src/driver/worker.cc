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
    constexpr static std::string_view usage_line = "clice worker [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    KANNOTATE(&stateful, description = "Run as stateful worker (default: stateless)")
    bool stateful = false;

    KANNOTATE(&worker_name, description = "Name the worker logs under")
    std::string worker_name = "worker";

    KANNOTATE(&log_dir, description = "Directory of the worker's log file (default: no log file)")
    std::string log_dir;
};

/// A worker logs as the pool relays it (run_stateless_worker_mode).
int run_worker(Worker& worker) {
    if(print_help(worker)) {
        return 0;
    }
#ifdef __APPLE__
    // See WorkerPool::spawn_process.
    exit_with_parent();
#endif
    // Started through /proc/self/exe on Linux, which `top` shows as
    // `exe`; elsewhere the name stays the program's.
    if(auto& arguments = sys::arguments(); !arguments.empty()) {
        sys::set_process_name(path::filename(arguments.front()));
    }
    // The pool relays each line of a worker's stderr as a warning: one
    // with no log file to write keeps its lower levels out of it.
    logging::options.stderr_level = logging::Level::warn;
    if(worker.stateful) {
        return run_stateful_worker_mode(worker.worker_name, worker.log_dir);
    }
    return run_stateless_worker_mode(worker.worker_name, worker.log_dir);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(run_worker, {.hidden = true});

}  // namespace

}  // namespace clice::driver
