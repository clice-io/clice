module;

#include "modules/prelude.h"

module clice;

import :unittests.server.worker_test_helpers;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :worker.protocol;

namespace clice::testing {
namespace {

ZEST_SUITE(CancelChain) {

// The master-side shape: an LSP handler task raced against its request
// token, passing the same token into the worker send. When the token
// fires, the send emits $/cancelRequest over IPC and the handler ends
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
        auto gate = w.gate(cp);
        group.spawn(wrapper());
        co_await w.parked(gate);
        source.cancel();
        w.release(gate);
        co_await group.join();

        // The worker must have seen the IPC cancel: an interrupted (or
        // never started) compile leaves the document without an AST.
        worker::DocumentSymbolParams qp;
        qp.path = src;
        // Hang detection: a reply that never comes fails here, before the
        // watchdog.
        auto symbols = co_await w.peer->send_request(qp, {.timeout = std::chrono::seconds(30)});
        ZASSERT(symbols);
        ZEXPECT(!symbols.value().has_value());

        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
    ZEXPECT(!handler_resumed);
    ZEXPECT(handler_cancelled);
}

// The scheduler's cooperative cancel of a stateless build is an IPC cancel
// whose answer the sender keeps awaiting (the slot must stay busy while the
// worker is): the stop flag it trips cuts the parse at the next declaration.
// The source ends in a pragma that crashes the worker (CLICE_TEST_PRAGMA_CRASH):
// only a parse run to its end reaches it.
ZEST_CASE(IpcCancelStopsBuild) {
    TempDir tmp;
    std::string text;
    text.reserve(1 << 22);
    for(int i = 0; i < 200'000; i += 1) {
        text += std::format("int v{};\n", i);
    }
    text += "#pragma clang __debug crash\n";
    tmp.touch("probe.cpp", text);
    auto src = tmp.path("probe.cpp");
    tmp.touch("small.cpp", "int small;\n");

    WorkerHandle w;
    w.env = {
        {.name = "CLICE_TEST_PRAGMA_CRASH", .value = "1"}
    };
    ZASSERT(w.spawn());

    bool test_done = false;
    w.run([&]() -> kota::task<> {
        worker::TURunParams bp;
        bp.index = true;
        bp.file = src;
        bp.directory = "/tmp";
        bp.arguments = make_args(src);

        kota::cancellation_source source;
        auto build = [&]() -> kota::task<> {
            auto result = co_await w.peer->send_request(bp, {.token = source.token()});
            ZASSERT(!result);
            ZEXPECT(result.error().code == worker::dispatch_errc::cancelled);
        };

        kota::task_group<> group;
        auto gate = w.gate(bp);
        group.spawn(build());
        co_await w.parked(gate);
        source.cancel();
        w.release(gate);
        co_await group.join();

        // The worker lives on: the parse stopped short of the pragma.
        bp.file = tmp.path("small.cpp");
        bp.arguments = make_args(bp.file);
        auto next = co_await w.peer->send_request(bp);
        ZASSERT(next);
        ZEXPECT(next->success);

        test_done = true;
        w.peer->close_output();
    });

    ZASSERT(test_done);
}

};  // ZEST_SUITE(CancelChain)

}  // namespace
}  // namespace clice::testing
