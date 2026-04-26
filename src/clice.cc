#include <csignal>
#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "server/service/agentic.h"
#include "server/service/master_server.h"
#include "server/worker/stateful_worker.h"
#include "server/worker/stateless_worker.h"
#include "support/logging.h"

#include "kota/deco/deco.h"

namespace clice {

using kota::deco::decl::KVStyle;

struct Options {
    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Running mode: pipe, socket, agentic, stateless-worker, stateful-worker",
           required = false)
    <std::string> mode;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Socket mode address", required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Agentic TCP port (0 = disabled)",
           required = false)
    <int> port = 0;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Record LSP input to file for replay testing",
           required = false)
    <std::string> record;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "File path for agentic queries",
           required = false)
    <std::string> path;

    // Internal options (passed from master to worker processes)
    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--worker-memory-limit", "--worker-memory-limit="},
           required = false)
    <std::uint64_t> worker_memory_limit;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--worker-name", "--worker-name="},
           required = false)
    <std::string> worker_name;

    DecoKV(style = KVStyle::JoinedOrSeparate, names = {"--log-dir", "--log-dir="}, required = false)
    <std::string> log_dir;

    DecoFlag(names = {"-h", "--help"}, help = "Show help message", required = false)
    help;

    DecoFlag(names = {"-v", "--version"}, help = "Show version", required = false)
    version;
};

}  // namespace clice

int main(int argc, const char** argv) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    auto args = kota::deco::util::argvify(argc, argv);
    auto result = kota::deco::cli::parse<clice::Options>(args);

    if(!result.has_value()) {
        LOG_ERROR("{}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false)) {
        kota::deco::cli::write_usage_for<clice::Options>(std::cout, "clice [OPTIONS]");
        return 0;
    }

    if(opts.version.value_or(false)) {
        std::println("clice version 0.1.0");
        return 0;
    }

    if(opts.log_level.has_value()) {
        auto level = spdlog::level::from_str(*opts.log_level);
        if(level == spdlog::level::off && *opts.log_level != "off") {
            std::println(stderr,
                         "unknown log level '{}', valid: trace, debug, info, warn, error, off",
                         *opts.log_level);
            return 1;
        }
        clice::logging::options.level = level;
    }

    if(!opts.mode.has_value()) {
        LOG_ERROR("--mode is required");
        return 1;
    }

    auto& mode = *opts.mode;

    auto worker_name = opts.worker_name.value_or("");
    auto log_dir = opts.log_dir.value_or("");

    if(mode == "stateless-worker") {
        return clice::run_stateless_worker_mode(worker_name.empty() ? "stateless-worker"
                                                                    : worker_name,
                                                log_dir);
    }

    if(mode == "stateful-worker") {
        auto mem_limit = opts.worker_memory_limit.value_or(4ULL * 1024 * 1024 * 1024);
        return clice::run_stateful_worker_mode(mem_limit,
                                               worker_name.empty() ? "stateful-worker"
                                                                   : worker_name,
                                               log_dir);
    }

    if(mode == "pipe" || mode == "socket") {
        clice::ServerOptions server_opts;
        server_opts.mode = mode;
        server_opts.host = opts.host.value_or("127.0.0.1");
        server_opts.port = opts.port.value_or(0);
        server_opts.self_path = argv[0];
        server_opts.record = opts.record.value_or("");
        return clice::run_server_mode(server_opts);
    }

    if(mode == "agentic") {
        auto host = opts.host.value_or("127.0.0.1");
        auto port = opts.port.value_or(0);
        auto path = opts.path.value_or("");
        if(port <= 0) {
            LOG_ERROR("--port is required for agentic mode");
            return 1;
        }
        if(path.empty()) {
            LOG_ERROR("--path is required for agentic mode");
            return 1;
        }
        return clice::run_agentic_mode(host, port, path);
    }

    LOG_ERROR("unknown mode '{}'", mode);
    return 1;
}
