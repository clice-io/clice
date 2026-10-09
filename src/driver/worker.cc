module;

#include "modules/prelude.h"

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :driver.driver;
import :support.process;
import :worker.stateful;
import :worker.stateless;

namespace clice::driver {

namespace {

using kota::deco::decl::KVStyle;

struct WorkerOptions {
    kota::deco::decl::HelpOption help;

    DecoFlag(names = {"--stateful"},
             help = "Run as stateful worker (default: stateless)",
             required = false)
    stateful;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--max-documents", "--max-documents="},
           help = "Max compiled documents kept before LRU eviction (stateful worker only)",
           required = false)
    <std::uint64_t> max_documents;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--worker-name", "--worker-name="},
           required = false)
    <std::string> worker_name;

    DecoKV(style = KVStyle::JoinedOrSeparate, names = {"--log-dir", "--log-dir="}, required = false)
    <std::string> log_dir;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--master-pid", "--master-pid="},
           required = false)
    <std::uint32_t> master_pid;
};

}  // namespace

void add_worker(kota::deco::cli::SubCommander& root) {
    auto cmd = kota::deco::cli::command<WorkerOptions>("clice worker [OPTIONS]");
    cmd.match_all([](WorkerOptions opts) {
        // A worker lives and dies with its master: an orphan answers no one
        // and nothing stops it when it hangs. Its own process group keeps a
        // terminal's Ctrl-C and SIGHUP for the master, which stops it.
        if(opts.master_pid) {
            exit_with_parent(*opts.master_pid);
        }
#ifndef _WIN32
        ::setpgid(0, 0);
#endif
        auto name = opts.worker_name.value_or("worker");
        auto log_dir = opts.log_dir.value_or("");
        if(opts.stateful) {
            auto max_docs = opts.max_documents.value_or(default_max_documents);
            return run_stateful_worker_mode(name, log_dir, max_docs);
        }
        return run_stateless_worker_mode(name, log_dir);
    });

    root.add({.name = "worker"}, std::move(cmd));
}

}  // namespace clice::driver
