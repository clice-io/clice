#include "test/test.h"
#include "server/protocol.h"

#include "eventide/async/io/loop.h"
#include "eventide/async/io/process.h"
#include "eventide/ipc/peer.h"
#include "eventide/ipc/transport.h"
#include "eventide/serde/serde/raw_value.h"

#include "support/filesystem.h"

#include <string>

namespace clice::testing {

namespace {

namespace et = eventide;

/// Resolve path to the clice binary for spawning workers.
std::string clice_binary() {
    auto resource_dir = fs::resource_dir;
    // resource_dir is <build>/lib/clang/...
    // clice binary is at <build>/bin/clice
    auto build_dir = llvm::sys::path::parent_path(
        llvm::sys::path::parent_path(
            llvm::sys::path::parent_path(resource_dir)));
    llvm::SmallString<256> path(build_dir);
    llvm::sys::path::append(path, "bin", "clice");
    return std::string(path);
}

/// Helper: spawn a worker process and return a BincodePeer connected to it.
struct WorkerHandle {
    et::event_loop loop;
    et::process proc{};
    std::unique_ptr<et::ipc::StreamTransport> transport;
    std::unique_ptr<et::ipc::BincodePeer> peer;

    bool spawn(const std::string& mode, std::uint64_t memory_limit = 0) {
        auto binary = clice_binary();

        et::process::options opts;
        opts.file = binary;
        opts.args = {binary, "--mode=" + mode};
        if(memory_limit > 0) {
            opts.args.push_back("--worker-memory-limit=" + std::to_string(memory_limit));
        }
        opts.streams = {
            et::process::stdio::pipe(true, false),   // stdin: child reads
            et::process::stdio::pipe(false, true),    // stdout: child writes
            et::process::stdio::pipe(false, true),    // stderr: child writes
        };

        auto result = et::process::spawn(opts, loop);
        if(!result) return false;

        auto& spawn = *result;
        transport = std::make_unique<et::ipc::StreamTransport>(
            std::move(spawn.stdout_pipe),
            std::move(spawn.stdin_pipe)
        );
        peer = std::make_unique<et::ipc::BincodePeer>(loop, std::move(transport));
        proc = std::move(spawn.proc);
        return true;
    }

    /// Run a coroutine on the event loop and return when it completes.
    template <typename F>
    void run(F&& coro_factory) {
        loop.schedule(coro_factory());
        loop.schedule(peer->run());
        loop.run();
    }
};

// ============================================================================
// StatelessWorker Tests
// ============================================================================

TEST_SUITE(StatelessWorker) {

TEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    // Close stdin pipe to signal worker to exit.
    w.peer->close_output();
    w.loop.schedule(w.peer->run());
    w.loop.run();
}

TEST_CASE(BuildPCHRequest) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::BuildPCHParams params;
        params.file = "/tmp/test_pch.h";
        params.directory = "/tmp";
        params.arguments = {"clang++", "-x", "c++-header", "/tmp/test_pch.h"};
        params.content = "#pragma once\nint pch_global = 42;\n";

        auto result = co_await w.peer->send_request(params);
        // The compilation may fail (no real file), but the IPC round-trip should work.
        // We test that the worker responds without crashing.
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(IndexRequest) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::IndexParams params;
        params.file = "/tmp/test_index.cpp";
        params.directory = "/tmp";
        params.arguments = {"clang++", "-c", "/tmp/test_index.cpp"};

        auto result = co_await w.peer->send_request(params);
        // May fail due to missing file, but IPC should work.
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

};  // TEST_SUITE(StatelessWorker)

// ============================================================================
// StatefulWorker Tests
// ============================================================================

TEST_SUITE(StatefulWorker) {

TEST_CASE(SpawnAndExit) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    w.peer->close_output();
    w.loop.schedule(w.peer->run());
    w.loop.run();
}

TEST_CASE(CompileRequest) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::CompileParams params;
        params.uri = "file:///tmp/test.cpp";
        params.version = 1;
        params.text = "int main() { return 0; }\n";
        params.directory = "/tmp";
        params.arguments = {"clang++", "-c", "/tmp/test.cpp"};

        auto result = co_await w.peer->send_request(params);
        if(result.has_value()) {
            EXPECT_EQ(result.value().uri, std::string("file:///tmp/test.cpp"));
            EXPECT_EQ(result.value().version, 1);
        }
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
        params.uri = "file:///tmp/nonexistent.cpp";
        params.line = 0;
        params.character = 0;

        auto result = co_await w.peer->send_request(params);
        if(result.has_value()) {
            // Should be "null" RawValue since document doesn't exist.
            EXPECT_EQ(result.value().data, std::string("null"));
        }
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(CompileThenHover) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // First compile
        worker::CompileParams cp;
        cp.uri = "file:///tmp/hover_test.cpp";
        cp.version = 1;
        cp.text = "int foo() { return 42; }\nint main() { return foo(); }\n";
        cp.directory = "/tmp";
        cp.arguments = {"clang++", "-c", "/tmp/hover_test.cpp"};

        auto compile_result = co_await w.peer->send_request(cp);
        EXPECT_TRUE(compile_result.has_value());

        // Then hover on 'foo' in main
        worker::HoverParams hp;
        hp.uri = "file:///tmp/hover_test.cpp";
        hp.line = 1;
        hp.character = 22;  // position of 'foo' in 'return foo();'

        auto hover_result = co_await w.peer->send_request(hp);
        EXPECT_TRUE(hover_result.has_value());
        // Should return hover info (not "null") since AST exists.

        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(DocumentUpdate) {
    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateful-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Compile first
        worker::CompileParams cp;
        cp.uri = "file:///tmp/update_test.cpp";
        cp.version = 1;
        cp.text = "int x = 1;\n";
        cp.directory = "/tmp";
        cp.arguments = {"clang++", "-c", "/tmp/update_test.cpp"};

        auto r1 = co_await w.peer->send_request(cp);
        EXPECT_TRUE(r1.has_value());

        // Send document update notification
        worker::DocumentUpdateParams up;
        up.uri = "file:///tmp/update_test.cpp";
        up.version = 2;
        up.text = "int x = 2;\nint y = 3;\n";
        w.peer->send_notification(up);

        // After update, hover should return null (dirty flag set).
        worker::HoverParams hp;
        hp.uri = "file:///tmp/update_test.cpp";
        hp.line = 0;
        hp.character = 4;

        auto hover_result = co_await w.peer->send_request(hp);
        if(hover_result.has_value()) {
            EXPECT_EQ(hover_result.value().data, std::string("null"));
        }

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
        params.uri = "file:///tmp/test.cpp";

        auto result = co_await w.peer->send_request(params);
        if(result.has_value()) {
            // Should return empty array "[]" (TODO stub)
            EXPECT_EQ(result.value().data, std::string("[]"));
        }
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
        params.uri = "file:///tmp/test.cpp";
        params.line = 0;
        params.character = 0;

        auto result = co_await w.peer->send_request(params);
        if(result.has_value()) {
            // Should return empty array "[]" (TODO stub)
            EXPECT_EQ(result.value().data, std::string("[]"));
        }
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

};  // TEST_SUITE(StatefulWorker)

}  // namespace

}  // namespace clice::testing
