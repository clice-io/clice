#include <csignal>
#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "eventide/async/async.h"
#include "eventide/deco/deco.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/recording_transport.h"
#include "eventide/ipc/transport.h"
#include "server/master_server.h"
#include "server/stateful_worker.h"
#include "server/stateless_worker.h"
#include "support/logging.h"

namespace clice {

using deco::decl::KVStyle;

struct Options {
    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Running mode: pipe, socket, stateless-worker, stateful-worker",
           required = false)
    <std::string> mode;

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Socket mode address", required = false)
    <std::string> host = "127.0.0.1";

    DecoKV(style = KVStyle::JoinedOrSeparate, help = "Socket mode port", required = false)
    <int> port = 50051;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level = "info";

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Record LSP input to file for replay testing",
           required = false)
    <std::string> record;

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
    // On POSIX systems, ignore SIGPIPE so that writing to a closed pipe
    // (e.g. when the LSP client disconnects) returns EPIPE instead of
    // killing the process.  This is standard practice for pipe-based servers.
    signal(SIGPIPE, SIG_IGN);
#endif

    auto args = deco::util::argvify(argc, argv);
    auto result = deco::cli::parse<clice::Options>(args);

    if(!result.has_value()) {
        LOG_ERROR("{}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false)) {
        deco::cli::write_usage_for<clice::Options>(std::cout, "clice [OPTIONS]");
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

    std::string self_path = argv[0];

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

    if(mode == "pipe") {
        clice::logging::stderr_logger("master", clice::logging::options);

        namespace et = eventide;
        et::event_loop loop;

        auto transport = et::ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            LOG_ERROR("failed to open stdio transport");
            return 1;
        }

        std::unique_ptr<et::ipc::Transport> final_transport = std::move(*transport);
        if(opts.record.has_value()) {
            final_transport =
                std::make_unique<et::ipc::RecordingTransport>(std::move(final_transport),
                                                              *opts.record);
        }

        et::ipc::JsonPeer peer(loop, std::move(final_transport));
        clice::MasterServer server(loop, peer, std::move(self_path));
        server.register_handlers();

        loop.schedule(peer.run());
        loop.run();
        return 0;
    }

    if(mode == "socket") {
        clice::logging::stderr_logger("master", clice::logging::options);

        namespace et = eventide;
        et::event_loop loop;

        auto host = opts.host.value_or("127.0.0.1");
        auto port = opts.port.value_or(50051);

        auto acceptor = et::tcp::listen(host, port, {}, loop);
        if(!acceptor) {
            LOG_ERROR("failed to listen on {}:{}", host, port);
            return 1;
        }

        LOG_INFO("Listening on {}:{} ...", host, port);

        auto task = [&]() -> et::task<> {
            auto client = co_await acceptor->accept();
            if(!client.has_value()) {
                LOG_ERROR("failed to accept connection");
                loop.stop();
                co_return;
            }

            LOG_INFO("Client connected");

            std::unique_ptr<et::ipc::Transport> transport =
                std::make_unique<et::ipc::StreamTransport>(std::move(client.value()));
            if(opts.record.has_value()) {
                transport = std::make_unique<et::ipc::RecordingTransport>(std::move(transport),
                                                                          *opts.record);
            }
            et::ipc::JsonPeer peer(loop, std::move(transport));
            clice::MasterServer server(loop, peer, std::string(self_path));
            server.register_handlers();

            co_await peer.run();
            peer.close();
            loop.stop();
        };

        loop.schedule(task());
        loop.run();
        return 0;
    }

    LOG_ERROR("unknown mode '{}'", mode);
    return 1;
}
