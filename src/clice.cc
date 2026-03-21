#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "server/master_server.h"
#include "server/stateless_worker.h"
#include "server/stateful_worker.h"

#include "eventide/async/io/loop.h"
#include "eventide/deco/macro.h"
#include "eventide/deco/runtime.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"

namespace clice {

struct Options {
    DecoKV(names = {"--mode"};
           help = "Running mode: pipe, socket, stateless-worker, stateful-worker";
           required = false;)
    <std::string> mode;

    DecoKV(names = {"--host"}; help = "Socket mode address"; required = false;)
    <std::string> host = "127.0.0.1";

    DecoKV(names = {"--port"}; help = "Socket mode port"; required = false;)
    <int> port = 50051;

    DecoKV(names = {"--stateful-worker-count"}; help = "Number of stateful workers";
           required = false;)
    <std::uint32_t> stateful_worker_count;

    DecoKV(names = {"--stateless-worker-count"}; help = "Number of stateless workers";
           required = false;)
    <std::uint32_t> stateless_worker_count;

    DecoKV(names = {"--worker-memory-limit"}; help = "Memory limit per stateful worker (bytes)";
           required = false;)
    <std::uint64_t> worker_memory_limit;

    DecoFlag(names = {"-h", "--help"}; help = "Show help message"; required = false;)
    help;

    DecoFlag(names = {"-v", "--version"}; help = "Show version"; required = false;)
    version;
};

}  // namespace clice

int main(int argc, const char** argv) {
    auto args = deco::util::argvify(argc, argv);
    auto result = deco::cli::parse<clice::Options>(args);

    if(!result.has_value()) {
        std::println(stderr, "error: {}", result.error().message);
        return 1;
    }

    auto& opts = result->options;

    if(opts.help.value_or(false)) {
        auto usage = deco::cli::Dispatcher<clice::Options>("clice [OPTIONS]");
        usage.usage(std::cout, true);
        return 0;
    }

    if(opts.version.value_or(false)) {
        std::println("clice version 0.1.0");
        return 0;
    }

    if(!opts.mode.has_value()) {
        std::println(stderr, "error: --mode is required");
        return 1;
    }

    auto& mode = *opts.mode;

    if(mode == "stateless-worker") {
        return clice::run_stateless_worker_mode();
    }

    if(mode == "stateful-worker") {
        auto mem_limit = opts.worker_memory_limit.value_or(4ULL * 1024 * 1024 * 1024);
        return clice::run_stateful_worker_mode(mem_limit);
    }

    if(mode == "pipe") {
        namespace et = eventide;
        et::event_loop loop;

        auto transport = et::ipc::StreamTransport::open_stdio(loop);
        if(!transport) {
            std::println(stderr, "error: failed to open stdio transport");
            return 1;
        }

        et::ipc::JsonPeer peer(loop, std::move(*transport));
        clice::MasterServer server(loop, peer);
        server.register_handlers();

        loop.schedule(peer.run());
        return loop.run();
    }

    std::println(stderr, "error: unknown mode '{}'", mode);
    return 1;
}
