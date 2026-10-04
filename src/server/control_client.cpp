#include "server/control_client.h"

#include <format>

#include "kota/async/async.h"
#include "kota/ipc/codec/json.h"
#include "kota/ipc/transport.h"

namespace clice::control {

namespace {

template <typename Params>
using Result = typename kota::ipc::protocol::RequestTraits<Params>::Result;

template <typename Params>
kota::task<> exchange(kota::ipc::JSONPeer& peer,
                      Params params,
                      std::expected<Result<Params>, std::string>& out) {
    auto result = co_await peer.send_request(std::move(params));
    if(result) {
        out = std::move(*result);
    } else {
        out = std::unexpected(std::format("request failed: {}", result.error().message));
    }
    peer.close();
}

template <typename Params>
kota::task<std::expected<Result<Params>, std::string>>
    session(const index::ServerEndpoint& endpoint, Params params) {
    auto& loop = kota::event_loop::current();
    auto transport =
        co_await kota::ipc::StreamTransport::connect_tcp(endpoint.host, endpoint.port, loop);
    if(!transport) {
        co_return std::unexpected(
            std::format("cannot connect to the clice server (pid {}) at {}:{}",
                        endpoint.pid,
                        endpoint.host,
                        endpoint.port));
    }
    kota::ipc::JSONPeer peer(loop, std::move(*transport));
    std::expected<Result<Params>, std::string> out =
        std::unexpected(std::string("the clice server closed the connection"));
    co_await kota::when_all(peer.run(), exchange(peer, std::move(params), out));
    co_return out;
}

template <typename Params>
std::expected<Result<Params>, std::string> request(const index::ServerEndpoint& endpoint,
                                                   Params params) {
    auto [ended] = kota::run(session(endpoint, std::move(params)));
    return std::move(*ended);
}

}  // namespace

std::expected<IndexResult, std::string> request_index(const index::ServerEndpoint& endpoint,
                                                      llvm::StringRef configuration) {
    return request(endpoint, IndexParams{.configuration = configuration.str()});
}

}  // namespace clice::control
