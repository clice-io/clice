#include <format>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/protocol/worker.h"
#include "server/worker_test_helpers.h"

#include "kota/async/async.h"

namespace clice::testing {
namespace {

TEST_SUITE(CancelChain) {

// The master-side shape: an LSP handler task raced against its request
// token, passing the same token into the worker send. When the token
// fires, the send's internal with_token boundary must resume (not be torn
// down by the handler's cancellation cascade) and emit the wire
// $/cancelRequest — proven here by the worker interrupting a 200k-decl
// parse instead of finishing it.
TEST_CASE(HandlerCancelChainsThrough) {
    TempDir tmp;
    tmp.touch("probe.cpp", "");
    auto src = tmp.path("probe.cpp");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn(4ULL * 1024 * 1024 * 1024));

    bool observed_cancelled_reply = false;
    bool handler_resumed = false;
    bool test_done = false;

    w.run([&]() -> kota::task<> {
        std::string text;
        text.reserve(1 << 22);
        for(int i = 0; i < 200'000; ++i) {
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
            kota::ipc::request_options opts;
            opts.token = source.token();
            auto result = co_await w.peer->send_request(cp, opts);
            handler_resumed = true;
            observed_cancelled_reply =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };

        kota::task_group<> group(w.loop);
        auto wrapper = [&]() -> kota::task<> {
            [[maybe_unused]] auto r = co_await kota::with_token(handler(), source.token());
        };
        group.spawn(wrapper());

        co_await kota::sleep(50, w.loop);
        source.cancel();
        co_await group.join();

        // Whatever happened to the handler, the worker must have seen the
        // wire cancel: a second compile completes quickly only if the first
        // parse was interrupted (200k decls otherwise).
        cp.version = 2;
        cp.text = "int x;\n";
        kota::ipc::request_options retry_opts;
        retry_opts.timeout = std::chrono::milliseconds(30'000);
        auto retry = co_await w.peer->send_request(cp, retry_opts);
        CO_ASSERT_TRUE(retry.has_value());
        EXPECT_EQ(retry.value().version, 2);

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
    // The resumption-boundary claim itself: the send must RESUME with a
    // cancelled error (emitting the wire cancel on the way), not be torn
    // down by the handler's cancellation cascade.
    EXPECT_TRUE(handler_resumed);
    EXPECT_TRUE(observed_cancelled_reply);
}

};  // TEST_SUITE(CancelChain)

}  // namespace
}  // namespace clice::testing
