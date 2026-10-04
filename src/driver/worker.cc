#include <cstdint>

#include "driver/driver.h"
#include "worker/stateful.h"
#include "worker/stateless.h"

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
};

}  // namespace

void add_worker(kota::deco::cli::SubCommander& root) {
    auto cmd = kota::deco::cli::command<WorkerOptions>("clice worker [OPTIONS]");
    cmd.match_all([](WorkerOptions opts) {
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
