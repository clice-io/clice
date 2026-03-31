#include <string>
#include <vector>

#include "test/test.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/protocol.h"
#include "server/worker_test_helpers.h"

namespace clice::testing {

namespace {

namespace et = eventide;

TEST_SUITE(StatefulWorker) {

TEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    w.peer->close_output();
    w.loop.schedule(w.peer->run());
    w.loop.run();
}

TEST_CASE(CompileRequest) {
    TempDir tmp;
    tmp.touch("compile_test.cpp", "int main() { return 0; }\n");
    auto src = tmp.path("compile_test.cpp");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::CompileParams params;
        params.path = src;
        params.version = 1;
        params.text = "int main() { return 0; }\n";
        params.directory = "/tmp";
        params.arguments = make_args(src);
        params.pch = {"", 0};
        params.pcms = {};

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(HoverWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Hover on a file that hasn't been compiled should return null.
        worker::HoverParams params;
        params.path = "/tmp/nonexistent.cpp";
        params.offset = 0;

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        // Should be "null" RawValue since document doesn't exist.
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(CompileThenHover) {
    std::string text = "int foo() { return 42; }\nint main() { return foo(); }\n";
    TempDir tmp;
    tmp.touch("hover_test.cpp", text);
    auto src = tmp.path("hover_test.cpp");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // First compile
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = text;
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto compile_result = co_await w.peer->send_request(cp);
        CO_ASSERT_TRUE(compile_result.has_value());

        // After successful compilation, hover should return info.
        // "int foo() { return 42; }\n" is 25 chars, then char 22 on line 1 = offset 47
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 47;  // position of 'foo' in 'return foo();'

        auto hover_result = co_await w.peer->send_request(hp);
        EXPECT_TRUE(hover_result.has_value());
        // Should return non-null hover info for 'foo'.
        EXPECT_NE(hover_result.value().data, std::string("null"));

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(DocumentUpdate) {
    TempDir tmp;
    tmp.touch("update_test.cpp", "int x = 1;\n");
    auto src = tmp.path("update_test.cpp");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Compile first
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int x = 1;\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto r1 = co_await w.peer->send_request(cp);
        CO_ASSERT_TRUE(r1.has_value());

        // Send document update notification
        worker::DocumentUpdateParams up;
        up.path = src;
        up.version = 2;
        up.text = "int x = 2;\nint y = 3;\n";
        w.peer->send_notification(up);

        // After update, hover still returns stale AST results (not null).
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 4;

        auto hover_result = co_await w.peer->send_request(hp);
        EXPECT_TRUE(hover_result.has_value());

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(CodeActionReturnsEmpty) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::CodeActionParams params;
        params.path = "/tmp/test.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        // Should return empty array "[]" (TODO stub)
        EXPECT_EQ(result.value().data, std::string("[]"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(GoToDefinitionReturnsEmpty) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::GoToDefinitionParams params;
        params.path = "/tmp/test.cpp";
        params.offset = 0;

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        // Should return empty array "[]" (TODO stub)
        EXPECT_EQ(result.value().data, std::string("[]"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(SemanticTokensWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::SemanticTokensParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(FoldingRangeWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::FoldingRangeParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(DocumentSymbolWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::DocumentSymbolParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(DocumentLinkWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::DocumentLinkParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(InlayHintsWithoutCompile) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::InlayHintsParams params;
        params.path = "/tmp/nonexistent.cpp";

        auto result = co_await w.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(MultipleSequentialRequests) {
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
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Compile first so feature requests return real data.
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int foo(int x) {\n    return x + 1;\n}\nint main() {\n    return foo(0);\n}\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto cr = co_await w.peer->send_request(cp);
        CO_ASSERT_TRUE(cr.has_value());

        // Now send multiple different feature requests sequentially.
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 4;  // 'foo' on line 0
        auto r1 = co_await w.peer->send_request(hp);
        EXPECT_TRUE(r1.has_value());

        worker::CodeActionParams cap;
        cap.path = src;
        auto r2 = co_await w.peer->send_request(cap);
        EXPECT_TRUE(r2.has_value());

        // 'foo' in 'return foo(0);' at line 4, char 11
        // lines: "int foo(int x) {\n"=17, "    return x + 1;\n"=18, "}\n"=2, "int main() {\n"=14
        // offset = 17+18+2+14+11 = 62
        worker::GoToDefinitionParams gdp;
        gdp.path = src;
        gdp.offset = 62;
        auto r3 = co_await w.peer->send_request(gdp);
        EXPECT_TRUE(r3.has_value());

        worker::SemanticTokensParams stp;
        stp.path = src;
        auto r4 = co_await w.peer->send_request(stp);
        EXPECT_TRUE(r4.has_value());

        worker::FoldingRangeParams frp;
        frp.path = src;
        auto r5 = co_await w.peer->send_request(frp);
        EXPECT_TRUE(r5.has_value());

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(MultipleDocuments) {
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
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Compile 3 different documents.
        for(int i = 0; i < 3; i++) {
            worker::CompileParams cp;
            cp.path = paths[i];
            cp.version = 1;
            cp.text = texts[i];
            cp.directory = "/tmp";
            cp.arguments = make_args(paths[i]);

            auto result = co_await w.peer->send_request(cp);
            EXPECT_TRUE(result.has_value());
        }

        // Hover on each document after compilation.
        for(int i = 0; i < 3; i++) {
            worker::HoverParams hp;
            hp.path = paths[i];
            hp.offset = 4;  // 'var_N'

            auto result = co_await w.peer->send_request(hp);
            EXPECT_TRUE(result.has_value());
        }

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(EvictNotification) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Send an evict notification — worker should remove the document without crashing.
        worker::EvictParams ep;
        ep.path = "/tmp/evict_test.cpp";
        w.peer->send_notification(ep);

        // Hover on the evicted document should return null (document doesn't exist).
        worker::HoverParams hp;
        hp.path = "/tmp/evict_test.cpp";
        hp.offset = 0;

        auto result = co_await w.peer->send_request(hp);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().data, std::string("null"));

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(SpawnWithMemoryLimit) {
    TempDir tmp;
    tmp.touch("memlimit_test.cpp", "int memlimit_var = 42;\n");
    auto src = tmp.path("memlimit_test.cpp");

    WorkerHandle w;
    // Spawn with a specific memory limit to test the CLI flag is accepted.
    ASSERT_TRUE(w.spawn("stateful-worker", 2ULL * 1024 * 1024 * 1024));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Compile first.
        worker::CompileParams cp;
        cp.path = src;
        cp.version = 1;
        cp.text = "int memlimit_var = 42;\n";
        cp.directory = "/tmp";
        cp.arguments = make_args(src);

        auto cr = co_await w.peer->send_request(cp);
        EXPECT_TRUE(cr.has_value());

        // Feature request should work after compilation.
        worker::HoverParams hp;
        hp.path = src;
        hp.offset = 4;  // 'memlimit_var'

        auto result = co_await w.peer->send_request(hp);
        EXPECT_TRUE(result.has_value());

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

};  // TEST_SUITE(StatefulWorker)

}  // namespace

}  // namespace clice::testing
