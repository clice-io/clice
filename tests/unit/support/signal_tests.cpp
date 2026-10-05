#include <vector>

#include "test/test.h"
#include "support/signal.h"

namespace clice::testing {
namespace {

ZEST_SUITE(Signal) {

ZEST_CASE(EmitCallsHandlers) {
    Signal<int> signal;
    int sum = 0;
    auto c1 = signal.connect([&](int v) { sum += v; });
    auto c2 = signal.connect([&](int v) { sum += v * 10; });
    signal.emit(3);
    ZASSERT(sum == 33);
}

ZEST_CASE(ConnectOrder) {
    Signal<> signal;
    std::vector<int> order;
    auto c1 = signal.connect([&] { order.push_back(1); });
    auto c2 = signal.connect([&] { order.push_back(2); });
    auto c3 = signal.connect([&] { order.push_back(3); });
    signal.emit();
    ZASSERT(order == (std::vector<int>{1, 2, 3}));
}

ZEST_CASE(DisconnectOnDestruction) {
    Signal<> signal;
    int calls = 0;
    {
        auto conn = signal.connect([&] { calls += 1; });
        signal.emit();
    }
    signal.emit();
    ZASSERT(calls == 1);
}

ZEST_CASE(ExplicitDisconnect) {
    Signal<> signal;
    int calls = 0;
    auto conn = signal.connect([&] { calls += 1; });
    conn.disconnect();
    signal.emit();
    ZASSERT(calls == 0);
}

ZEST_CASE(MoveTransfersConnection) {
    Signal<> signal;
    int calls = 0;
    Signal<>::Connection held;
    {
        auto conn = signal.connect([&] { calls += 1; });
        held = std::move(conn);
    }
    signal.emit();
    ZASSERT(calls == 1);
    held.disconnect();
    signal.emit();
    ZASSERT(calls == 1);
}

ZEST_CASE(ConnectionOutlivesSignal) {
    Signal<>::Connection conn;
    {
        Signal<> signal;
        conn = signal.connect([] {});
    }
    // Disconnecting after the signal is destroyed must be a no-op.
    conn.disconnect();
}

ZEST_CASE(EmitWithoutSubscribers) {
    Signal<int> signal;
    signal.emit(42);
}

};  // ZEST_SUITE(Signal)

}  // namespace
}  // namespace clice::testing
