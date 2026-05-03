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
    DecoKV(
        style = KVStyle::JoinedOrSeparate,
        help =
            "Running mode: pipe, socket, daemon, relay, agentic, stateless-worker, stateful-worker",
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

    DecoKV(
        style = KVStyle::JoinedOrSeparate,
        help =
            "Agentic method (compileCommand, symbolSearch, definition, references, "
            "documentSymbols, readSymbol, callGraph, typeHierarchy, projectFiles, "
            "fileDeps, impactAnalysis, status, shutdown)",
        required = false)
    <std::string> method;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Symbol name for agentic queries",
           required = false)
    <std::string> name;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Search query for symbolSearch",
           required = false)
    <std::string> query;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Line number for position-based lookup",
           required = false)
    <int> line;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Direction: callers/callees or supertypes/subtypes",
           required = false)
    <std::string> direction;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Unix domain socket path for daemon mode",
           required = false)
    <std::string> socket;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory for daemon mode",
           required = false)
    <std::string> workspace;

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

    if(mode == "daemon") {
        auto workspace = opts.workspace.value_or("");
        if(workspace.empty()) {
            LOG_ERROR("--workspace is required for daemon mode");
            return 1;
        }

        clice::DaemonOptions daemon_opts;
        daemon_opts.socket_path = opts.socket.value_or("");
        daemon_opts.workspace = std::move(workspace);
        daemon_opts.self_path = argv[0];
        return clice::run_daemon_mode(daemon_opts);
    }

    if(mode == "agentic") {
        auto port = opts.port.value_or(0);
        if(port <= 0) {
            LOG_ERROR("--port is required for agentic mode");
            return 1;
        }
        clice::AgenticQueryOptions aq;
        aq.host = opts.host.value_or("127.0.0.1");
        aq.port = port;
        aq.method = opts.method.value_or("compileCommand");
        aq.path = opts.path.value_or("");
        aq.name = opts.name.value_or("");
        aq.query = opts.query.value_or("");
        aq.line = opts.line.value_or(0);
        aq.direction = opts.direction.value_or("");
        return clice::run_agentic_mode(aq);
    }

    if(mode == "relay") {
        auto socket = opts.socket.value_or("");
        return clice::run_relay_mode(socket);
    }

    LOG_ERROR("unknown mode '{}'", mode);
    return 1;
}
