#include <csignal>
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
    ZASSERT(w.spawn(true));

    w.run([]() -> kota::task<> { co_return; });
}

ZEST_CASE(CompileRequest) {
    TempDir tmp;
    tmp.touch("compile_test.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("compile_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

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
        ZASSERT(result);
        ZEXPECT(result.value().version == 1);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(CancelledCompileFreesStrand) {
    TempDir tmp;
    tmp.touch("cancel_test.cpp", "");
    auto src = tmp.path("cancel_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

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
        ZEXPECT(first_failed);

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
        ZASSERT(retry);
        ZEXPECT(retry.value().version == 2);

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(WireCancelInterruptsCompile) {
    TempDir tmp;
    tmp.touch("interrupt.cpp", "");
    auto src = tmp.path("interrupt.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

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

        // The cancel chases the request down the pipe and flips the stop
        // flag mid-parse; the request still waits for the worker's answer.
        // An interrupted parse leaves the document without an AST, which
        // the next query reports — the content, not the clock, is the
        // assertion.
        kota::cancellation_source source;
        bool cancelled = false;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await w.peer->send_request(
                cp,
                {.token = source.token(), .timeout = std::chrono::seconds(30)});
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        group.spawn(sender());
        co_await kota::sleep(20, w.loop);
        source.cancel();
        co_await group.join();
        ZASSERT(cancelled);

        worker::QueryParams qp;
        qp.kind = worker::QueryKind::DocumentSymbol;
        qp.path = src;
        auto symbols = co_await w.peer->send_request(qp);
        ZASSERT(symbols);
        ZEXPECT(symbols.value().data == "null");

        test_done = true;
    });

    ZASSERT(test_done);
}

#ifndef _WIN32
ZEST_CASE(UnstartedCompileWakesQueries) {
    TempDir tmp;
    tmp.touch("unstarted.cpp", "");
    auto src = tmp.path("unstarted.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int x;\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        cp.pch = {"", 0};
        cp.pcms = {};

        // Stopped, the worker reads the request and its cancel in one read
        // once it resumes, so the compile's task never starts.
        ZASSERT(!w.proc.kill(SIGSTOP));
        kota::cancellation_source source;
        bool cancelled = false;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await w.peer->send_request(
                cp,
                {.token = source.token(), .timeout = std::chrono::seconds(30)});
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        group.spawn(sender());
        co_await kota::sleep(50, w.loop);
        source.cancel();
        co_await kota::sleep(50, w.loop);
        ZASSERT(!w.proc.kill(SIGCONT));
        co_await group.join();
        ZASSERT(cancelled);

        worker::QueryParams qp;
        qp.kind = worker::QueryKind::DocumentSymbol;
        qp.path = src;
        auto symbols = co_await w.peer->send_request(qp, {.timeout = std::chrono::seconds(30)});
        ZASSERT(symbols);
        ZEXPECT(symbols.value().data == "null");

        test_done = true;
    });

    ZASSERT(test_done);
}
#endif

ZEST_CASE(CancelledQueryFreesStrand) {
    TempDir tmp;
    tmp.touch("query_cancel.cpp", "");
    auto src = tmp.path("query_cancel.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

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
        ZASSERT(compiled);

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
        ZASSERT(retry);

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(HoverWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::Hover;
        params.path = "/tmp/nonexistent.cpp";
        params.offset = 0;

        auto result = co_await w.peer->send_request(params);
        // An unknown document is no empty answer: the master hears it was
        // evicted (or never sent) and compiles it again.
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(CompileThenHover) {
    std::string text = "int foo() { return 42; }\nint main() { return foo(); }\n";
    TempDir tmp;
    tmp.touch("hover_test.cpp", text);
    auto src = tmp.path("hover_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

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
        ZASSERT(compile_result);

        // After successful compilation, hover should return info.
        // "int foo() { return 42; }\n" is 25 chars, then char 22 on line 1 = offset 47
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = src;
        hp.offset = 47;  // position of 'foo' in 'return foo();'

        auto hover_result = co_await w.peer->send_request(hp);
        ZEXPECT(hover_result);
        // Should return non-null hover info for 'foo'.
        ZEXPECT(hover_result.value().data != std::string("null"));

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(InvalidUTF8Replaced) {
    std::string text = "/// caf\xe9\nint foo();\nint x = foo();\n";
    TempDir tmp;
    tmp.touch("latin1.cpp", text);
    auto src = tmp.path("latin1.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    std::string hover;
    w.run([&]() -> kota::task<> {
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = text;
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        ZASSERT(co_await w.peer->send_request(cp));

        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = src;
        hp.offset = static_cast<std::uint32_t>(text.find("foo()"));
        auto result = co_await w.peer->send_request(hp);
        ZASSERT(result);
        hover = result.value().data;
    });

    ZEXPECT(
        hover.contains("caf"
                       "\xef\xbf\xbd"));
}

ZEST_CASE(CodeActionWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::CodeActionParams params;
        params.path = "/tmp/test.cpp";
        params.range = {0, 0};

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(SemanticTokensWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::SemanticTokens;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(FoldingRangeWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::FoldingRangeParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(DocumentSymbolWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::DocumentSymbol;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(DocumentLinkWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::DocumentLinkParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(InlayHintsWithoutCompile) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> kota::task<> {
        worker::QueryParams params;
        params.kind = worker::QueryKind::InlayHints;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);
        test_done = true;
    });

    ZASSERT(test_done);
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
    ZASSERT(w.spawn(true));

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
        ZASSERT(cr);

        // Now send multiple different feature requests sequentially.
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = src;
        hp.offset = 4;  // 'foo' on line 0
        auto r1 = co_await w.peer->send_request(hp);
        ZEXPECT(r1);

        worker::CodeActionParams cap;
        cap.path = src;
        cap.range = {4, 4};
        auto r2 = co_await w.peer->send_request(cap);
        ZEXPECT(r2);

        worker::QueryParams stp;
        stp.kind = worker::QueryKind::SemanticTokens;
        stp.path = src;
        auto r3 = co_await w.peer->send_request(stp);
        ZEXPECT(r3);

        worker::FoldingRangeParams frp;
        frp.path = src;
        auto r4 = co_await w.peer->send_request(frp);
        ZEXPECT(r4);

        test_done = true;
    });

    ZASSERT(test_done);
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
    ZASSERT(w.spawn(true));

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
            ZEXPECT(result);
        }

        // Hover on each document after compilation.
        for(int i = 0; i < 3; i++) {
            worker::QueryParams hp;
            hp.kind = worker::QueryKind::Hover;
            hp.path = paths[i];
            hp.offset = 4;  // 'var_N'

            auto result = co_await w.peer->send_request(hp);
            ZEXPECT(result);
        }

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(EvictNotification) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

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
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);

        test_done = true;
    });

    ZASSERT(test_done);
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
    ZASSERT(w.spawn(true, /*max_documents=*/2));

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
            ZEXPECT(result);
        }

        // The third compile overflowed the 2-document cap: the least
        // recently used document was evicted and announced to the master.
        worker::QueryParams hp;
        hp.kind = worker::QueryKind::Hover;
        hp.path = paths[0];
        hp.offset = 4;  // 'var_0'

        auto result = co_await w.peer->send_request(hp);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::document_unloaded);

        test_done = true;
    });

    ZASSERT(test_done);
    ZASSERT(evicted == std::vector<std::string>{paths[0]});
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
    ZASSERT(w.spawn(true, /*max_documents=*/1));

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
            ZEXPECT(result);
        };
        co_await kota::when_all(compile(0), compile(1));
        test_done = true;
    });

    ZASSERT(test_done);
    ZASSERT(evicted == std::vector<std::string>{documents[1].first});
}

};  // ZEST_SUITE(StatefulWorker)

}  // namespace

}  // namespace clice::testing
