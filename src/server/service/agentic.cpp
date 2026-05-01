#include "server/service/agentic.h"

#include <memory>
#include <print>
#include <string>

#include "server/protocol/agentic.h"
#include "support/logging.h"

#include "kota/async/async.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"

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

}  // namespace clice
