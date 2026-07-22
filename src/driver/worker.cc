#include <cstdint>

#include "driver/driver.h"
#include "server/worker/stateful_worker.h"
#include "server/worker/stateless_worker.h"

namespace clice::driver {

namespace {

using kota::deco::decl::KVStyle;

struct WorkerOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoFlag(names = {"--stateful"},
             help = "Run as stateful worker (default: stateless)",
             required = false)
    stateful;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--memory-limit", "--memory-limit="},
           help = "Memory limit in bytes (stateful worker only)",
           required = false)
    <std::uint64_t> memory_limit;

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

auto make_command() {
    return kota::deco::cli::command<WorkerOptions>("clice worker [OPTIONS]");
}

}  // namespace

void add_worker(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](WorkerOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           auto name = opts.worker_name.value_or("worker");
           auto log_dir = opts.log_dir.value_or("");
           if(opts.stateful) {
               auto limit = opts.memory_limit.value_or(4ULL * 1024 * 1024 * 1024);
               auto max_docs = opts.max_documents.value_or(default_max_documents);
               exit_code = run_stateful_worker_mode(limit, name, log_dir, max_docs);
           } else {
               exit_code = run_stateless_worker_mode(name, log_dir);
           }
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "worker"}, std::move(cmd));
}

}  // namespace clice::driver
