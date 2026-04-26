#include "server/service/agentic.h"

#include <print>
#include <string>

#include "protocol/agentic.h"
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

static kota::task<> agentic_client(kota::event_loop& loop,
                                   int& exit_code,
                                   std::string host,
                                   int port,
                                   std::string path) {
    auto transport = co_await kota::ipc::StreamTransport::connect_tcp(host, port, loop);
    if(!transport) {
        LOG_ERROR("failed to connect to {}:{}", host, port);
        co_return;
    }

    kota::ipc::JsonPeer peer(loop, std::move(*transport));
    co_await kota::when_all(peer.run(), agentic_request(peer, exit_code, std::move(path)));
}

int run_agentic_mode(llvm::StringRef host, int port, llvm::StringRef path) {
    logging::stderr_logger("agentic", logging::options);

    kota::event_loop loop;
    int exit_code = 1;
    loop.schedule(agentic_client(loop, exit_code, host.str(), port, path.str()));
    loop.run();
    return exit_code;
}

}  // namespace clice
