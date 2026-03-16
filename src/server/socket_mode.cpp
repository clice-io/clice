#include "server/socket_mode.h"

#include "server/server.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "eventide/async/async.h"
#include "eventide/async/io/stream.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"

namespace clice {

namespace et = eventide;

static et::task<> handle_connection(et::tcp_socket conn,
                                    et::event_loop& loop,
                                    const Options& options) {
    auto transport = std::make_unique<et::ipc::StreamTransport>(std::move(conn));
    auto peer = et::ipc::JsonPeer(loop, std::move(transport));

    Server server(loop, peer, options);
    server.register_callbacks();
    co_await peer.run();
}

int run_socket_mode(const Options& options) {
    if(auto result = fs::init_resource_dir(options.self_path); !result) {
        LOG_WARN("Failed to init resource dir: {}", result.error());
    }

    et::event_loop loop;

    auto acceptor_result = et::tcp_socket::listen(options.host, options.port, {}, loop);
    if(!acceptor_result) {
        LOG_ERROR("Failed to listen on {}:{}: {}",
                  options.host, options.port,
                  acceptor_result.error().message());
        return 1;
    }

    auto acceptor = std::move(*acceptor_result);
    LOG_INFO("Listening on {}:{}", options.host, options.port);

    loop.schedule([&]() -> et::task<> {
        while(true) {
            auto conn = co_await acceptor.accept();
            if(!conn.has_value()) {
                LOG_ERROR("Accept failed: {}", conn.error().message());
                continue;
            }
            loop.schedule(handle_connection(std::move(*conn), loop, options));
        }
    }());

    loop.run();
    return 0;
}

}  // namespace clice
