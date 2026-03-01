#pragma once

#include <expected>
#include <memory>
#include <string>

#include "eventide/async/loop.h"
#include "eventide/jsonrpc/peer.h"
#include "server/runtime.h"

namespace clice::server {

namespace et = eventide;
namespace jsonrpc = et::jsonrpc;

class MasterServer {
public:
    MasterServer(et::event_loop& loop, jsonrpc::Peer& peer, const Options& options);
    ~MasterServer();

    MasterServer(MasterServer&&) noexcept;
    MasterServer& operator=(MasterServer&&) noexcept;

    MasterServer(const MasterServer&) = delete;
    MasterServer& operator=(const MasterServer&) = delete;

    auto start() -> std::expected<void, std::string>;
    [[nodiscard]] auto exit_code() const -> int;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace clice::server
