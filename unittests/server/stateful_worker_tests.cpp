module clice;

import :feature.hover;
import :support.json;
import :syntax.source_text;
import :unittests.server.worker_test_helpers;
import :unittests.utils;
import :worker.protocol;

namespace clice::testing {

namespace {

ZEST_SUITE(StatefulWorker) {

ZEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ZASSERT(w.spawn(true));

    w.run([]() -> async::task<> { co_return; });
}

ZEST_CASE(CompileRequest) {
    TempDir tmp;
    tmp.touch("compile_test.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("compile_test.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> async::task<> {
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

    w.run([&]() -> async::task<> {
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int v0;\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        cp.pch = {"", 0};
        cp.pcms = {};

        async::cancellation_source source;
        ipc::request_options opts;
        opts.token = source.token();

        bool first_failed = false;
        async::task_group<> group;
        auto sender = [&]() -> async::task<> {
            auto result = co_await w.peer->send_request(cp, opts);
            first_failed = !result.has_value();
        };
        // The compile is under way, holding the strand, when the cancel
        // arrives.
        auto gate = w.gate(cp);
        group.spawn(sender());
        co_await w.parked(gate);
        source.cancel();
        w.release(gate);
        co_await group.join();
        ZEXPECT(first_failed);

        // The strand must be free again: a second compile of the same
        // document completes instead of hanging behind the cancelled one's
        // never-released lock (the guard releases it when the cancelled
        // handler's frame unwinds).
        cp.version = 2;
        cp.text = "int done;\n";
        ipc::request_options retry_opts;
        // Hang detection: a strand still held fails the retry here,
        // attributably, rather than the whole run at its timeout.
        retry_opts.timeout = chrono::milliseconds(30'000);
        auto retry = co_await w.peer->send_request(cp, retry_opts);
        ZASSERT(retry);
        ZEXPECT(retry.value().version == 2);

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(IpcCancelInterruptsCompile) {
    TempDir tmp;
    tmp.touch("interrupt.cpp", "");
    auto src = tmp.path("interrupt.cpp");

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;

    w.run([&]() -> async::task<> {
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
        async::cancellation_source source;
        bool cancelled = false;
        async::task_group<> group;
        auto sender = [&]() -> async::task<> {
            // Hang detection: a reply that never comes fails here, before
            // the watchdog.
            auto result = co_await w.peer->send_request(
                cp,
                {.token = source.token(), .timeout = chrono::seconds(30)});
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        auto gate = w.gate(cp);
        group.spawn(sender());
        co_await w.parked(gate);
        source.cancel();
        w.release(gate);
        co_await group.join();
        ZASSERT(cancelled);

        worker::DocumentSymbolParams qp;
        qp.path = src;
        auto symbols = co_await w.peer->send_request(qp);
        ZASSERT(symbols);
        ZEXPECT(!symbols.value().has_value());

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

    w.run([&]() -> async::task<> {
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
        async::cancellation_source source;
        bool cancelled = false;
        async::task_group<> group;
        auto sender = [&]() -> async::task<> {
            // Hang detection: a reply that never comes fails here, before
            // the watchdog.
            auto result = co_await w.peer->send_request(
                cp,
                {.token = source.token(), .timeout = chrono::seconds(30)});
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        group.spawn(sender());
        // No event: the request's write into the stopped worker's pipe
        // ends unannounced.
        co_await async::sleep(50, w.loop);
        source.cancel();
        // No event: so does the cancel's.
        co_await async::sleep(50, w.loop);
        ZASSERT(!w.proc.kill(SIGCONT));
        co_await group.join();
        ZASSERT(cancelled);

        worker::DocumentSymbolParams qp;
        qp.path = src;
        // Hang detection: a reply that never comes fails here, before the
        // watchdog.
        auto symbols = co_await w.peer->send_request(qp, {.timeout = chrono::seconds(30)});
        ZASSERT(symbols);
        ZEXPECT(!symbols.value().has_value());

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

    w.run([&]() -> async::task<> {
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
        worker::SemanticTokensParams qp;
        qp.path = src;

        async::cancellation_source source;
        ipc::request_options opts;
        opts.token = source.token();

        async::task_group<> group;
        auto sender = [&]() -> async::task<> {
            [[maybe_unused]] auto result = co_await w.peer->send_request(qp, opts);
        };
        group.spawn(sender());
        source.cancel();
        co_await group.join();

        ipc::request_options retry_opts;
        // Hang detection: a strand still held fails the retry here.
        retry_opts.timeout = chrono::milliseconds(30'000);
        auto retry = co_await w.peer->send_request(qp, retry_opts);
        ZASSERT(retry);

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

    w.run([&]() -> async::task<> {
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
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 47;  // position of 'foo' in 'return foo();'

        auto hover_result = co_await w.peer->send_request(hp);
        ZASSERT(hover_result);
        // Should return the card of 'foo'.
        ZASSERT(hover_result.value().has_value());
        ZEXPECT(hover_result.value()->name == "foo");

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
    w.run([&]() -> async::task<> {
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = text;
        cp.directory = "/tmp";
        cp.arguments = make_args(src);
        ZASSERT(co_await w.peer->send_request(cp));

        worker::HoverParams hp;
        hp.path = src;
        hp.offset = static_cast<std::uint32_t>(text.find("foo()"));
        auto result = co_await w.peer->send_request(hp);
        ZASSERT(result);
        ZASSERT(result.value().has_value());
        // The card crosses to the master as it is; its JSON for the client
        // replaces what is not UTF-8.
        SourceText source(text);
        hover = to_raw(feature::to_protocol(*result.value(),
                                            protocol::MarkupKind::Markdown,
                                            source.positions(),
                                            PositionEncoding::UTF16))
                    .data;
    });

    ZEXPECT(
        hover.contains("caf"
                       "\xef\xbf\xbd"));
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

    w.run([&]() -> async::task<> {
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
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 4;  // 'foo' on line 0
        auto r1 = co_await w.peer->send_request(hp);
        ZEXPECT(r1);

        worker::CodeActionParams cap;
        cap.path = src;
        cap.range = {4, 4};
        auto r2 = co_await w.peer->send_request(cap);
        ZEXPECT(r2);

        worker::SemanticTokensParams stp;
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

    w.run([&]() -> async::task<> {
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
            worker::HoverParams hp;
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

    w.run([&]() -> async::task<> {
        // Evicting a document the worker does not hold changes nothing.
        worker::EvictParams ep;
        ep.path = "/tmp/evict_test.cpp";
        w.peer->send_notification(ep);

        worker::CompileParams cp;
        cp.path = "/tmp/evict_test.cpp";
        cp.version = 1;
        cp.text = "int x = 1;\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(cp.path);
        ZEXPECT(co_await w.peer->send_request(cp));

        worker::HoverParams hp;
        hp.path = cp.path;
        hp.offset = 4;
        ZEXPECT(co_await w.peer->send_request(hp));

        test_done = true;
    });

    ZASSERT(test_done);
}

ZEST_CASE(EvictsOnlyWhenTold) {
    // The worker keeps every document the master's pool sends it, past the
    // sixteen it once kept on its own, until it is told to evict one; an
    // evicted document compiles again.
    TempDir tmp;
    std::vector<std::string> paths;
    for(int i = 0; i < 20; i += 1) {
        auto name = std::format("kept_{}.cpp", i);
        tmp.touch(name, std::format("int var_{} = {};\n", i, i));
        paths.push_back(tmp.path(name));
    }

    WorkerHandle w;
    ZASSERT(w.spawn(true));

    bool test_done = false;
    w.run([&]() -> async::task<> {
        auto compile = [&](std::size_t i) -> async::task<bool> {
            worker::CompileParams cp;
            cp.path = paths[i];
            cp.version = 1;
            cp.text = std::format("int var_{} = {};\n", i, i);
            cp.directory = "/tmp";
            cp.arguments = make_args(paths[i]);
            auto result = co_await w.peer->send_request(cp);
            co_return result.has_value() && result->status == worker::CompileStatus::Done;
        };
        auto hover = [&](std::size_t i) -> async::task<bool> {
            worker::HoverParams hp;
            hp.path = paths[i];
            hp.offset = 4;  // 'var_N'
            auto result = co_await w.peer->send_request(hp);
            co_return result.has_value() && result->has_value();
        };
        for(std::size_t i = 0; i < paths.size(); i += 1) {
            ZEXPECT(co_await compile(i));
        }
        for(std::size_t i = 0; i < paths.size(); i += 1) {
            ZEXPECT(co_await hover(i));
        }

        w.peer->send_notification(worker::EvictParams{paths[0]});
        ZEXPECT(co_await compile(0));
        ZEXPECT(co_await hover(0));

        test_done = true;
    });

    ZASSERT(test_done);
}

};  // ZEST_SUITE(StatefulWorker)

}  // namespace

}  // namespace clice::testing
