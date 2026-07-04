#include <vector>

#include "test/test.h"
#include "support/signal.h"

namespace clice::testing {
namespace {

TEST_SUITE(Signal) {

TEST_CASE(EmitCallsHandlers) {
    Signal<int> signal;
    int sum = 0;
    auto c1 = signal.connect([&](int v) { sum += v; });
    auto c2 = signal.connect([&](int v) { sum += v * 10; });
    signal.emit(3);
    ASSERT_EQ(sum, 33);
}

TEST_CASE(ConnectOrder) {
    Signal<> signal;
    std::vector<int> order;
    auto c1 = signal.connect([&] { order.push_back(1); });
    auto c2 = signal.connect([&] { order.push_back(2); });
    auto c3 = signal.connect([&] { order.push_back(3); });
    signal.emit();
    ASSERT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST_CASE(DisconnectOnDestruction) {
    Signal<> signal;
    int calls = 0;
    {
        auto conn = signal.connect([&] { calls += 1; });
        signal.emit();
    }
    signal.emit();
    ASSERT_EQ(calls, 1);
}

TEST_CASE(ExplicitDisconnect) {
    Signal<> signal;
    int calls = 0;
    auto conn = signal.connect([&] { calls += 1; });
    conn.disconnect();
    signal.emit();
    ASSERT_EQ(calls, 0);
}

TEST_CASE(MoveTransfersConnection) {
    Signal<> signal;
    int calls = 0;
    Signal<>::Connection held;
    {
        auto conn = signal.connect([&] { calls += 1; });
        held = std::move(conn);
    }
    signal.emit();
    ASSERT_EQ(calls, 1);
    held.disconnect();
    signal.emit();
    ASSERT_EQ(calls, 1);
}

TEST_CASE(ConnectionOutlivesSignal) {
    Signal<>::Connection conn;
    {
        Signal<> signal;
        conn = signal.connect([] {});
    }
    // Disconnecting after the signal is destroyed must be a no-op.
    conn.disconnect();
}

TEST_CASE(EmitWithoutSubscribers) {
    Signal<int> signal;
    signal.emit(42);
}

};  // TEST_SUITE(Signal)

}  // namespace
}  // namespace clice::testing
