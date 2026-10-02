#include <cstdint>

#include "driver/driver.h"
#include "worker/stateful.h"
#include "worker/stateless.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Process.h"

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
               auto max_docs = opts.max_documents.value_or(default_max_documents);
               // Lets integration tests drive eviction through a real server.
               if(auto value = llvm::sys::Process::GetEnv("CLICE_TEST_MAX_DOCUMENTS")) {
                   llvm::StringRef(*value).getAsInteger(10, max_docs);
               }
               exit_code = run_stateful_worker_mode(name, log_dir, max_docs);
           } else {
               exit_code = run_stateless_worker_mode(name, log_dir);
           }
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "worker"}, std::move(cmd));
}

}  // namespace clice::driver
