module;

#include "modules/prelude.h"

module clice;

import :worker.gate;
import :worker.protocol;

namespace clice {

namespace {

struct Gate {
    std::uint64_t id;
    std::string tag;
    bool parked = false;
    bool released = false;
    /// Reports the park from the parked thread to the loop; destroyed with
    /// the gate's release, since it keeps the loop alive.
    kota::relay relay;
};

/// The gates the master placed. The mutex also orders a relay's send
/// before its destruction.
struct Gates {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Gate> standing;
    /// Read without the mutex: while no gate stands, a request's start
    /// takes no lock.
    std::atomic<std::size_t> count = 0;
    kota::ipc::BincodePeer* peer = nullptr;
};

Gates gates;

void erase_gate(std::vector<Gate>::iterator it) {
    gates.standing.erase(it);
    gates.count.store(gates.standing.size(), std::memory_order_relaxed);
}

}  // namespace

void install_test_gates(kota::ipc::BincodePeer& peer, kota::event_loop& loop) {
    gates.peer = &peer;
    peer.on_notification([&loop](const worker::GateParams& params) {
        std::lock_guard lock(gates.mutex);
        gates.standing.push_back(
            {.id = params.id, .tag = params.tag, .relay = loop.create_relay()});
        gates.count.store(gates.standing.size(), std::memory_order_relaxed);
    });
    peer.on_notification([](const worker::GateReleaseParams& params) {
        std::lock_guard lock(gates.mutex);
        auto it =
            llvm::find_if(gates.standing, [&](const Gate& gate) { return gate.id == params.id; });
        if(it == gates.standing.end()) {
            return;
        }
        if(!it->parked) {
            erase_gate(it);
            return;
        }
        it->released = true;
        it->relay = {};
        gates.changed.notify_all();
    });
}

void park_at_test_gate(llvm::StringRef tag) {
    if(gates.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::unique_lock lock(gates.mutex);
    auto it = llvm::find_if(gates.standing,
                            [&](const Gate& gate) { return !gate.parked && gate.tag == tag; });
    if(it == gates.standing.end()) {
        return;
    }
    it->parked = true;
    auto id = it->id;
    it->relay.send([id] { gates.peer->send_notification(worker::GateParkedParams{id}); });
    auto find = [id] {
        return llvm::find_if(gates.standing, [id](const Gate& gate) { return gate.id == id; });
    };
    gates.changed.wait(lock, [&] { return find()->released; });
    erase_gate(find());
}

void release_test_gates() {
    std::lock_guard lock(gates.mutex);
    std::erase_if(gates.standing, [](const Gate& gate) { return !gate.parked; });
    for(auto& gate: gates.standing) {
        gate.released = true;
        gate.relay = {};
    }
    gates.count.store(gates.standing.size(), std::memory_order_relaxed);
    gates.changed.notify_all();
}

}  // namespace clice
