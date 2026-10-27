module;

#include "support/prelude.h"

module clice;

import :driver.driver;
import :support.logging;
import :support.process;
import :vfs.path;
import :worker.stateful;
import :worker.stateless;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;

    DecoFlag(names = {"--stateful"},
             help = "Run as stateful worker (default: stateless)",
             required = false)
    stateful;

    DecoKV(help = "Max compiled documents kept before LRU eviction (stateful worker only)",
           required = false)
    <std::uint64_t> max_documents;

    DecoKV(help = "Name the worker logs under (default: worker)", required = false)
    <std::string> worker_name;

    DecoKV(help = "Directory of the worker's log file (default: no log file)", required = false)
    <std::string> log_dir;
};

}  // namespace

int run_worker(std::span<std::string> args) {
    auto cmd = kota::deco::cli::command<Options>("clice worker [OPTIONS]");
    cmd.match_all([](Options opts) {
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
        auto name = opts.worker_name.value_or("worker");
        auto log_dir = opts.log_dir.value_or("");
        if(opts.stateful) {
            auto max_docs = opts.max_documents.value_or(default_max_documents);
            return run_stateful_worker_mode(name, log_dir, max_docs);
        }
        return run_stateless_worker_mode(name, log_dir);
    });
    return cmd(args);
}

}  // namespace clice::driver
