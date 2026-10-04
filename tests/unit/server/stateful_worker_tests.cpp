#include <format>
#include <string>
#include <vector>

#include "test/test.h"
#include "server/worker_test_helpers.h"
#include "worker/protocol.h"

#include "kota/codec/json/json.h"

namespace clice::testing {

namespace {

ZEST_SUITE(StatefulWorker) {

ZEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    w.peer->close_output();
    w.loop.schedule(w.peer->run());
    w.loop.run();
}

ZEST_CASE(CompileRequest) {
    TempDir tmp;
    tmp.touch("compile_test.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("compile_test.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = src;
        params.version = 1;
        params.text = "int main() { return 0; }\n";
        params.directory = "/tmp";
        params.arguments = make_args(src);
        params.pch = {"", 0};
        params.pcms = {};

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(result);
        EXPECT(result.value().version == 1);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(CancelledCompileFreesStrand) {
    TempDir tmp;
    tmp.touch("cancel_test.cpp", "");
    auto src = tmp.path("cancel_test.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // A deterministically slow TU: two hundred thousand trivial
        // declarations keep the parse far past the cancellation tick on any
        // hardware — two consecutive macOS runners completed a whole
        // four-STL-header compile roundtrip inside 20ms — while the
        // per-declaration stop poll keeps the cancelled remainder cheap.
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
        kota::ipc::request_options opts;
        opts.token = source.token();

        bool first_failed = false;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await w.peer->send_request(cp, opts);
            first_failed = !result.has_value();
        };
        group.spawn(sender());
        // One short tick: enough for the request bytes to flush, far below
        // any parse of four STL headers. The $/cancelRequest then chases
        // the request down the same pipe, and the single-threaded worker
        // processes it while the handler is parked on the pool await — the
        // compile cannot have replied first, so the cancel deterministically
        // lands mid-flight (a 150ms window lost this race on a warm macOS
        // runner that compiled the TU faster).
        co_await kota::sleep(20, w.loop);
        source.cancel();
        co_await group.join();
        EXPECT(first_failed);

        // The strand must be free again: a second compile of the same
        // document completes instead of hanging behind the cancelled one's
        // never-released lock (the guard releases it when the cancelled
        // handler's frame unwinds). A held strand blocks a compile of any
        // size, so the retry uses a trivial TU — re-parsing the 200k
        // declarations here once blew the bound on a slow runner — and the
        // bound makes a regression a clean, attributable failure instead
        // of a CI-wide timeout.
        cp.version = 2;
        cp.text = "int done;\n";
        kota::ipc::request_options retry_opts;
        retry_opts.timeout = std::chrono::milliseconds(30'000);
        auto retry = co_await w.peer->send_request(cp, retry_opts);
        CO_ASSERT(retry);
        EXPECT(retry.value().version == 2);

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(CancelNotificationInterruptsCompile) {
    TempDir tmp;
    tmp.touch("interrupt.cpp", "");
    auto src = tmp.path("interrupt.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

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

        // The notification chases the request down the pipe and flips the
        // stop flag mid-parse. Unlike a wire cancel the request runs to a
        // reply, and an interrupted parse reports no deps and no index —
        // the reply's content, not the clock, is the assertion.
        std::optional<worker::CompileResult> reply;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            kota::ipc::request_options opts;
            opts.timeout = std::chrono::milliseconds(30'000);
            auto result = co_await w.peer->send_request(cp, opts);
            if(result.has_value()) {
                reply = std::move(result.value());
            }
        };
        group.spawn(sender());
        co_await kota::sleep(20, w.loop);
        w.peer->send_notification(worker::CancelCompileParams{src});
        co_await group.join();

        CO_ASSERT(reply);
        EXPECT(reply->version == 1);
        EXPECT(reply->deps.empty());
        EXPECT(reply->tu_index_data.empty());

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(CancelledQueryFreesStrand) {
    TempDir tmp;
    tmp.touch("query_cancel.cpp", "");
    auto src = tmp.path("query_cancel.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int value() { return 42; }\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        cp.pch = {"", 0};
        cp.pcms = {};
        auto compiled = co_await w.peer->send_request(cp);
        CO_ASSERT(compiled);

        // Queries take the strand through with_ast_or: a cancelled query
        // must release it on unwind like a cancelled compile does.
        worker::QueryParams qp;
        qp.kind = worker::QueryKind::SemanticTokens;
        qp.path = src;

        kota::cancellation_source source;
        kota::ipc::request_options opts;
        opts.token = source.token();

        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            [[maybe_unused]] auto result = co_await w.peer->send_request(qp, opts);
        };
        group.spawn(sender());
        source.cancel();
        co_await group.join();

        // Bounded: a still-locked strand fails this cleanly via timeout.
        kota::ipc::request_options retry_opts;
        retry_opts.timeout = std::chrono::milliseconds(30'000);
        auto retry = co_await w.peer->send_request(qp, retry_opts);
        CO_ASSERT(retry);

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(HoverWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::Hover;
        params.path = "/tmp/nonexistent.cpp";
        params.offset = 0;

        auto result = co_await w.peer->send_request(params);
        // An unknown document is no empty answer: the master hears it was
        // evicted (or never sent) and compiles it again.
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(CompileThenHover) {
    std::string text = "int foo() { return 42; }\nint main() { return foo(); }\n";
    TempDir tmp;
    tmp.touch("hover_test.cpp", text);
    auto src = tmp.path("hover_test.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // First compile
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = text;
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto compile_result = co_await w.peer->send_request(cp);
        CO_ASSERT(compile_result);

        // After successful compilation, hover should return info.
        // "int foo() { return 42; }\n" is 25 chars, then char 22 on line 1 = offset 47
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = src;
        hp.offset = 47;  // position of 'foo' in 'return foo();'

        auto hover_result = co_await w.peer->send_request(hp);
        EXPECT(hover_result);
        // Should return non-null hover info for 'foo'.
        EXPECT(hover_result.value().data != std::string("null"));

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(CodeActionWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CodeActionParams params;
        params.path = "/tmp/test.cpp";
        params.range = {0, 0};

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(SemanticTokensWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::SemanticTokens;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(FoldingRangeWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::FoldingRangeParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(DocumentSymbolWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::DocumentSymbol;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(DocumentLinkWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::DocumentLinkParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(InlayHintsWithoutCompile) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::InlayHints;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(MultipleSequentialRequests) {
    TempDir tmp;
    tmp.touch("seq_test.cpp",
              "int foo(int x) {\n"
              "    return x + 1;\n"
              "}\n"
              "int main() {\n"
              "    return foo(0);\n"
              "}\n");
    auto src = tmp.path("seq_test.cpp");

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // Compile first so feature requests return real data.
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int foo(int x) {\n    return x + 1;\n}\nint main() {\n    return foo(0);\n}\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto cr = co_await w.peer->send_request(cp);
        CO_ASSERT(cr);

        // Now send multiple different feature requests sequentially.
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = src;
        hp.offset = 4;  // 'foo' on line 0
        auto r1 = co_await w.peer->send_request(hp);
        EXPECT(r1);

        worker::CodeActionParams cap;
        cap.path = src;
        cap.range = {4, 4};
        auto r2 = co_await w.peer->send_request(cap);
        EXPECT(r2);

        worker::QueryParams stp;
        stp.kind = worker::QueryKind::SemanticTokens;
        stp.path = src;
        auto r3 = co_await w.peer->send_request(stp);
        EXPECT(r3);

        worker::FoldingRangeParams frp;
        frp.path = src;
        auto r4 = co_await w.peer->send_request(frp);
        EXPECT(r4);

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(MultipleDocuments) {
    TempDir tmp;
    std::vector<std::string> paths;
    std::vector<std::string> texts;
    for(int i = 0; i < 3; i++) {
        auto name = "multi_" + std::to_string(i) + ".cpp";
        auto text = "int var_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        tmp.touch(name, text);
        paths.push_back(tmp.path(name));
        texts.push_back(text);
    }

    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // Compile 3 different documents.
        for(int i = 0; i < 3; i++) {
            worker::CompileParams cp;
            cp.path = paths[i];
            cp.version = 1;
            cp.text = texts[i];
            cp.directory = "/tmp";
            cp.arguments = make_args(paths[i]);

            auto result = co_await w.peer->send_request(cp);
            EXPECT(result);
        }

        // Hover on each document after compilation.
        for(int i = 0; i < 3; i++) {
            worker::QueryParams hp;
            hp.kind = worker::QueryKind::Hover;
            hp.path = paths[i];
            hp.offset = 4;  // 'var_N'

            auto result = co_await w.peer->send_request(hp);
            EXPECT(result);
        }

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(EvictNotification) {
    WorkerHandle w;
    ASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        // Send an evict notification — worker should remove the document without crashing.
        worker::EvictParams ep;
        ep.path = "/tmp/evict_test.cpp";
        w.peer->send_notification(ep);

        // Hover on the evicted document reports it unloaded.
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = "/tmp/evict_test.cpp";
        hp.offset = 0;

        auto result = co_await w.peer->send_request(hp);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);

        test_done = true;
    });

    ASSERT(test_done);
}

ZEST_CASE(DocumentLimitEvicts) {
    TempDir tmp;
    std::vector<std::string> paths;
    std::vector<std::string> texts;
    for(int i = 0; i < 3; i++) {
        auto name = "evict_" + std::to_string(i) + ".cpp";
        auto text = "int var_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        tmp.touch(name, text);
        paths.push_back(tmp.path(name));
        texts.push_back(text);
    }

    WorkerHandle w;
    ASSERT(w.spawn(true, /*max_documents=*/2));

    std::vector<std::string> evicted;
    w.peer->on_notification(
        [&](const worker::EvictedParams& params) { evicted.push_back(params.path); });

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        for(int i = 0; i < 3; i++) {
            worker::CompileParams cp;
            cp.path = paths[i];
            cp.version = 1;
            cp.text = texts[i];
            cp.directory = "/tmp";
            cp.arguments = make_args(paths[i]);

            auto result = co_await w.peer->send_request(cp);
            EXPECT(result);
        }

        // The third compile overflowed the 2-document cap: the least
        // recently used document was evicted and announced to the master.
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = paths[0];
        hp.offset = 4;  // 'var_0'

        auto result = co_await w.peer->send_request(hp);
        CO_ASSERT(!result.has_value());
        EXPECT(result.error().code == worker::dispatch_errc::document_unloaded);

        test_done = true;
    });

    ASSERT(test_done);
    ASSERT(evicted == std::vector<std::string>{paths[0]});
}

ZEST_CASE(BusyDocumentsStay) {
    // A document in flight is never evicted: the master would hear of the
    // eviction while the compile is still to land and compile it again.
    // The large document's compile is still running when the small one's
    // lands over the cap; only the small one, idle by the time the large
    // one lands, may go.
    TempDir tmp;
    std::string large = "#include <vector>\n";
    for(int i = 0; i < 5000; i += 1) {
        large += std::format("std::vector<int> v_{};\n", i);
    }
    tmp.touch("large.cpp", large);
    tmp.touch("small.cpp", "int x;\n");
    std::vector<std::pair<std::string, std::string>> documents = {
        {tmp.path("large.cpp"), large     },
        {tmp.path("small.cpp"), "int x;\n"},
    };

    WorkerHandle w;
    ASSERT(w.spawn(true, /*max_documents=*/1));

    std::vector<std::string> evicted;
    w.peer->on_notification(
        [&](const worker::EvictedParams& params) { evicted.push_back(params.path); });

    bool test_done = false;
    w.run([&]() -> kota::task<> {
        auto compile = [&](std::size_t i) -> kota::task<> {
            worker::CompileParams cp;
            cp.path = documents[i].first;
            cp.version = 1;
            cp.text = documents[i].second;
            cp.directory = "/tmp";
            cp.arguments = make_args(cp.path);
            auto result = co_await w.peer->send_request(cp);
            EXPECT(result);
        };
        co_await kota::when_all(compile(0), compile(1));
        test_done = true;
    });

    ASSERT(test_done);
    ASSERT(evicted == std::vector<std::string>{documents[1].first});
}

};  // ZEST_SUITE(StatefulWorker)

}  // namespace

}  // namespace clice::testing
