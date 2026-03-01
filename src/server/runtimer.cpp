#include <memory>
#include <print>

#include "eventide/async/loop.h"
#include "eventide/jsonrpc/peer.h"
#include "eventide/jsonrpc/transport.h"
#include "server/master_server.h"
#include "server/runtime.h"
#include "server/worker_runtime.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice::server {

namespace {

namespace et = eventide;
namespace jsonrpc = et::jsonrpc;

auto run_master_session(et::event_loop& loop,
                        std::unique_ptr<jsonrpc::Transport> transport,
                        const Options& options) -> int {
    if(auto resource_dir = fs::init_resource_dir(options.self_path); !resource_dir) {
        LOG_WARN("failed to initialize resource-dir: {}", resource_dir.error());
    }

    jsonrpc::Peer peer(loop, std::move(transport));
    MasterServer server(loop, peer, options);

    auto started = server.start();
    if(!started) {
        LOG_ERROR("failed to start worker pool: {}", started.error());
        return 1;
    }

    loop.schedule(peer.run());
    auto loop_status = loop.run();
    if(loop_status != 0) {
        return loop_status;
    }
    return server.exit_code();
}

}  // namespace

auto run_pipe_mode(const Options& options) -> int {
    et::event_loop loop;
    auto stdio = jsonrpc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        LOG_ERROR("failed to open stdio transport: {}", stdio.error());
        return 1;
    }

    std::unique_ptr<jsonrpc::Transport> transport = std::move(*stdio);
    return run_master_session(loop, std::move(transport), options);
}

auto run_socket_mode(const Options& options) -> int {
    et::event_loop loop;

    auto listener_result = et::tcp_socket::listen(options.host, options.port, {}, loop);
    if(!listener_result) {
        LOG_ERROR("failed to listen on {}:{}: {}",
                  options.host,
                  options.port,
                  listener_result.error().message());
        return 1;
    }

    auto listener = std::move(*listener_result);
    auto accept_task = listener.accept();
    loop.schedule(accept_task);
    auto loop_status = loop.run();
    if(loop_status != 0) {
        return loop_status;
    }

    auto accepted = accept_task.value();
    listener = {};
    if(!accepted || !accepted->has_value()) {
        if(accepted && !accepted->has_value()) {
            LOG_ERROR("failed to accept connection: {}", accepted->error().message());
        } else {
            LOG_ERROR("failed to accept connection: unknown error");
        }
        return 1;
    }

    auto socket = std::move(**accepted);
    auto stream = et::stream(std::move(socket));
    auto transport = std::make_unique<jsonrpc::StreamTransport>(std::move(stream));
    return run_master_session(loop, std::move(transport), options);
}

auto run_worker_mode(const Options& options) -> int {
    if(auto resource_dir = fs::init_resource_dir(options.self_path); !resource_dir) {
        std::println(stderr, "failed to initialize resource-dir: {}", resource_dir.error());
    }

    et::event_loop loop;
    auto stdio = jsonrpc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        std::println(stderr, "failed to open worker stdio transport: {}", stdio.error());
        return 1;
    }

    jsonrpc::Peer peer(loop, std::move(*stdio));
    std::unique_ptr<WorkerRuntime> runtime;
    if(options.worker_role == WorkerRole::Stateless) {
        runtime = make_stateless_worker_runtime(peer);
    } else {
        runtime = make_stateful_worker_runtime(peer, options.worker_document_capacity);
    }

    loop.schedule(peer.run());
    return loop.run();
}

}  // namespace clice::server
