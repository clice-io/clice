#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace clice {

/// A typed multi-subscriber signal for domain→transport notification.
///
/// Design contract: signals only wake subscribers up — the data they react
/// to must already live in server state (e.g. Session fields), so a missed
/// signal is harmless and a subscriber that connects late simply reads the
/// current state on the next emission. Handlers therefore carry identity
/// arguments (which session?), never payloads that would be lost when no
/// subscriber exists.
///
/// Threading: event-loop thread only, no locks. emit() calls handlers
/// synchronously in connect order. Connecting or disconnecting while an
/// emission is in progress is not allowed (asserted, not snapshot-copied).
template <typename... Args>
class Signal {
    struct Slot {
        std::uint64_t id;
        std::function<void(Args...)> handler;
    };

    struct State {
        std::vector<Slot> slots;
        std::uint64_t next_id = 1;
        bool emitting = false;
    };

public:
    /// RAII connection handle — destruction disconnects the handler, so a
    /// subscriber that stores it as a member is unregistered automatically
    /// when it is destroyed. Outliving the signal is safe: disconnecting
    /// after the signal is gone is a no-op.
    class Connection {
    public:
        Connection() = default;

        Connection(Connection&& other) noexcept :
            state(std::exchange(other.state, {})), id(std::exchange(other.id, 0)) {}

        Connection& operator=(Connection&& other) noexcept {
            if(this != &other) {
                disconnect();
                state = std::exchange(other.state, {});
                id = std::exchange(other.id, 0);
            }
            return *this;
        }

        ~Connection() {
            disconnect();
        }

        void disconnect() {
            auto locked = state.lock();
            state.reset();
            if(!locked) {
                return;
            }
            assert(!locked->emitting && "disconnect during emit is not allowed");
            std::erase_if(locked->slots, [this](const Slot& slot) { return slot.id == id; });
        }

    private:
        friend class Signal;

        Connection(std::weak_ptr<State> state, std::uint64_t id) :
            state(std::move(state)), id(id) {}

        std::weak_ptr<State> state;
        std::uint64_t id = 0;
    };

    Signal() : state(std::make_shared<State>()) {}

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    [[nodiscard]] Connection connect(std::function<void(Args...)> handler) {
        assert(!state->emitting && "connect during emit is not allowed");
        auto id = state->next_id++;
        state->slots.push_back(Slot{id, std::move(handler)});
        return Connection(state, id);
    }

    /// Invoke all connected handlers synchronously, in connect order.
    void emit(Args... args) const {
        assert(!state->emitting && "reentrant emit is not allowed");
        state->emitting = true;
        for(const auto& slot: state->slots) {
            slot.handler(args...);
        }
        state->emitting = false;
    }

private:
    std::shared_ptr<State> state;
};

}  // namespace clice
