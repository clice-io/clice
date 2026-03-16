#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "eventide/async/async.h"
#include "eventide/ipc/peer.h"
#include "eventide/language/protocol.h"

namespace clice {

namespace et = eventide;
namespace lsp = eventide::ipc::protocol;

/// Reports progress to the LSP client using $/progress notifications.
/// Manages the lifecycle: create token → begin → report → end.
class ProgressReporter {
public:
    ProgressReporter(et::ipc::JsonPeer& peer, et::event_loop& loop);

    et::task<bool> begin(const std::string& title,
                         const std::string& message = "",
                         bool cancellable = false);

    void report(const std::string& message,
                std::optional<int> percentage = {});

    void end(const std::string& message = "");

    bool active() const { return active_; }

private:
    et::ipc::JsonPeer& peer_;
    et::event_loop& loop_;
    std::string token_;
    bool active_ = false;
    std::uint32_t next_token_ = 0;
};

}  // namespace clice
