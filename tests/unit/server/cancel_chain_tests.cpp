#include <chrono>
#include <format>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/worker_test_helpers.h"
#include "worker/protocol.h"

#include "kota/async/async.h"

namespace clice::testing {
namespace {

ZEST_SUITE(CancelChain) {

// The master-side shape: an LSP handler task raced against its request
// token, passing the same token into the worker send. When the token
// fires, the send emits the wire $/cancelRequest and the handler ends
// cancelled once the worker has answered — proven here by the worker
// interrupting a 200k-decl parse instead of finishing it.
ZEST_CASE(HandlerCancelChainsThrough) {
    TempDir tmp;
    tmp.touch("probe.cpp", "");
    auto src = tmp.path("probe.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool handler_resumed = false;
    bool handler_cancelled = false;
    bool test_done = false;

    w.run([&]() -> kota::task<> {
        std::string text;
        text.reserve(1 << 22);
        for(int i = 0; i < 200'000; i += 1) {
            text += std::format("int v{};\n", i);
        }

        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = std::move(text);
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        cp.pch = {"", 0};
        cp.pcms = {};

        kota::cancellation_source source;

        // handler-shaped: the task itself is raced against the token, and
        // the send inside passes the same token down.
        auto handler = [&]() -> kota::task<> {
            [[maybe_unused]] auto result =
                co_await w.peer->send_request(cp, {.token = source.token()});
            handler_resumed = true;
        };

        kota::task_group<> group;
        auto wrapper = [&]() -> kota::task<> {
            auto result = co_await kota::with_token(handler(), source.token());
            handler_cancelled = result.is_cancelled();
        };
        group.spawn(wrapper());

        co_await kota::sleep(50, w.loop);
        source.cancel();
        co_await group.join();

        // The worker must have seen the wire cancel: an interrupted (or
        // never started) compile leaves the document without an AST.
        worker::QueryParams qp;
        qp.kind = worker::QueryKind::DocumentSymbol;
        qp.path = src;
        auto symbols = co_await w.peer->send_request(qp, {.timeout = std::chrono::seconds(30)});
        ZASSERT(symbols);
        ZEXPECT(symbols.value().data == "null");

        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
    ZEXPECT(!handler_resumed);
    ZEXPECT(handler_cancelled);
}

// The scheduler's cooperative cancel of a stateless build is a wire cancel
// whose answer the sender keeps awaiting (the slot must stay busy while the
// worker is): the stop flag it trips makes that answer arrive at the next
// declaration boundary instead of after the whole TU.
ZEST_CASE(WireCancelStopsBuild) {
    TempDir tmp;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; i += 1) {
        text += std::format("int v{};\n", i);
    }
    tmp.touch("probe.cpp", text);
    auto src = tmp.path("probe.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn());

    bool test_done = false;
    w.run([&]() -> kota::task<> {
        worker::TURunParams bp;
        bp.index = true;
        bp.file = src;
        bp.directory = "/tmp";
        bp.arguments = make_args(src);

        kota::cancellation_source source;
        std::chrono::steady_clock::time_point cancelled_at;
        auto build = [&]() -> kota::task<> {
            auto result = co_await w.peer->send_request(bp, {.token = source.token()});
            auto waited = std::chrono::steady_clock::now() - cancelled_at;
            ZASSERT(!result);
            ZEXPECT(result.error().code == worker::dispatch_errc::cancelled);
            // An uninterrupted worker would index all 200k decls first.
            ZEXPECT(waited < std::chrono::seconds(5));
        };

        kota::task_group<> group;
        group.spawn(build());

        co_await kota::sleep(50, w.loop);
        cancelled_at = std::chrono::steady_clock::now();
        source.cancel();
        co_await group.join();

        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

};  // ZEST_SUITE(CancelChain)

}  // namespace
}  // namespace clice::testing
