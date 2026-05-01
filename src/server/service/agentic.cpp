#include "server/service/agentic.h"

#include <memory>
#include <print>
#include <string>

#include "server/protocol/agentic.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"
#include "llvm/Support/Path.h"

namespace clice {

static kota::task<> agentic_request(kota::ipc::JsonPeer& peer, int& exit_code, std::string path) {
    auto result =
        co_await peer.send_request(agentic::CompileCommandParams{.path = std::move(path)});

    if(!result) {
        LOG_ERROR("request failed: {}", result.error().message);
    } else {
        auto json = kota::codec::json::to_string<kota::ipc::lsp_config>(*result);
        std::println("{}", json ? *json : "null");
        exit_code = 0;
    }

    peer.close();
}

static kota::task<> agentic_client(int& exit_code,
                                   std::unique_ptr<kota::ipc::JsonPeer>& peer_out,
                                   std::string host,
                                   int port,
                                   std::string path) {
    auto& loop = kota::event_loop::current();
    auto transport = co_await kota::ipc::StreamTransport::connect_tcp(host, port, loop);
    if(!transport) {
        LOG_ERROR("failed to connect to {}:{}", host, port);
        co_return;
    }

    peer_out = std::make_unique<kota::ipc::JsonPeer>(loop, std::move(*transport));
    co_await kota::when_all(peer_out->run(),
                            agentic_request(*peer_out, exit_code, std::move(path)));
}

int run_agentic_mode(llvm::StringRef host, int port, llvm::StringRef path) {
    logging::stderr_logger("agentic", logging::options);

    kota::event_loop loop;
    int exit_code = 1;
    std::unique_ptr<kota::ipc::JsonPeer> peer;
    loop.schedule(agentic_client(exit_code, peer, host.str(), port, path.str()));
    loop.run();
    return exit_code;
}

static kota::task<> relay_forward(kota::ipc::Transport& from, kota::ipc::Transport& to) {
    while(true) {
        auto msg = co_await from.read_message();
        if(!msg)
            break;
        co_await to.write_message(*msg);
    }
    to.close();
}

static kota::task<> relay_main(kota::event_loop& loop, std::string socket_path) {
    auto stdio = kota::ipc::StreamTransport::open_stdio(loop);
    if(!stdio) {
        LOG_ERROR("failed to open stdio transport");
        loop.stop();
        co_return;
    }

    auto conn = co_await kota::pipe::connect(socket_path, {}, loop);
    if(!conn) {
        LOG_ERROR("failed to connect to {}", socket_path);
        loop.stop();
        co_return;
    }

    auto socket = std::make_unique<kota::ipc::StreamTransport>(std::move(*conn));

    co_await kota::when_all(relay_forward(**stdio, *socket), relay_forward(*socket, **stdio));
    loop.stop();
}

static std::string default_socket_path() {
    llvm::SmallString<128> home;
    if(!llvm::sys::path::home_directory(home))
        return "/tmp/clice.sock";
    llvm::sys::path::append(home, ".clice", "clice.sock");
    return home.str().str();
}

int run_relay_mode(llvm::StringRef socket_path) {
    logging::stderr_logger("relay", logging::options);

    auto path = socket_path.empty() ? default_socket_path() : socket_path.str();

    kota::event_loop loop;
    loop.schedule(relay_main(loop, std::move(path)));
    loop.run();
    return 0;
}

}  // namespace clice
