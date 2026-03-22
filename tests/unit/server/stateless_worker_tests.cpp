#include <string>
#include <vector>

#include "test/test.h"
#include "eventide/serde/bincode/bincode.h"
#include "eventide/serde/serde/raw_value.h"
#include "server/protocol.h"
#include "server/worker_test_helpers.h"

namespace clice::testing {

namespace {

namespace et = eventide;

// ============================================================================
// Bincode Serialization Tests
// ============================================================================

TEST_SUITE(BincodeRoundTrip) {

TEST_CASE(CompileParamsRoundTrip) {
    namespace bincode = eventide::serde::bincode;

    worker::CompileParams params;
    params.path = "/tmp/test.cpp";
    params.version = 1;
    params.text = "int main() { return 0; }";
    params.directory = "/tmp";
    params.arguments = {"clang++", "-c", "test.cpp"};
    params.pch = {"", 0};
    params.pcms = {};

    auto bytes = bincode::to_bytes(params);
    ASSERT_TRUE(bytes.has_value());

    worker::CompileParams result;
    auto status =
        bincode::from_bytes(std::span<const std::byte>(bytes->data(), bytes->size()), result);
    ASSERT_TRUE(status.has_value());

    EXPECT_EQ(result.path, params.path);
    EXPECT_EQ(result.version, params.version);
    EXPECT_EQ(result.text, params.text);
    EXPECT_EQ(result.directory, params.directory);
    EXPECT_EQ(result.arguments.size(), params.arguments.size());
}

TEST_CASE(CompileResultRoundTrip) {
    namespace bincode = eventide::serde::bincode;

    worker::CompileResult result;
    result.version = 1;
    result.diagnostics = {};  // empty
    result.memory_usage = 0;

    auto bytes = bincode::to_bytes(result);
    ASSERT_TRUE(bytes.has_value());

    worker::CompileResult decoded;
    auto status =
        bincode::from_bytes(std::span<const std::byte>(bytes->data(), bytes->size()), decoded);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(decoded.version, result.version);
}

};  // TEST_SUITE(BincodeRoundTrip)

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
    TempFile hdr("test_pch.h", "#pragma once\nint pch_global = 42;\n");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::BuildPCHParams params;
        params.file = hdr.path;
        params.directory = "/tmp";
        params.arguments =
            {"clang++", "-resource-dir", fs::resource_dir, "-x", "c++-header", hdr.path};
        params.content = "#pragma once\nint pch_global = 42;\n";

        auto result = co_await w.peer->send_request(params);
        EXPECT_TRUE(result.has_value());
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(IndexRequest) {
    TempFile src("test_index.cpp", "int indexed_var = 1;\n");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::IndexParams params;
        params.file = src.path;
        params.directory = "/tmp";
        params.arguments = make_args(src.path);

        auto result = co_await w.peer->send_request(params);
        EXPECT_TRUE(result.has_value());
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

};  // TEST_SUITE(StatelessWorker)

// ============================================================================
// StatelessWorker Extended Tests
// ============================================================================

TEST_SUITE(StatelessWorkerExtended) {

TEST_CASE(BuildPCMRequest) {
    TempFile src("test_module.cppm",
                 "export module test_module;\nexport int module_func() { return 1; }\n");

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::BuildPCMParams params;
        params.file = src.path;
        params.directory = "/tmp";
        params.arguments =
            {"clang++", "-resource-dir", fs::resource_dir, "-std=c++20", "--precompile", src.path};
        params.module_name = "test_module";

        auto result = co_await w.peer->send_request(params);
        EXPECT_TRUE(result.has_value());
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(CompletionRequest) {
    std::string text = "int foo = 1;\nint bar = fo";
    TempFile src("completion_test.cpp", text);

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::CompletionParams params;
        params.path = src.path;
        params.version = 1;
        params.text = text;
        params.directory = "/tmp";
        params.arguments = make_args(src.path);
        params.offset = 25;  // after "fo" in "int bar = fo" (13 + 12)

        auto result = co_await w.peer->send_request(params);
        EXPECT_TRUE(result.has_value());
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(SignatureHelpRequest) {
    std::string text = "void foo(int a, int b) {}\nint main() { foo(";
    TempFile src("sighelp_test.cpp", text);

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        worker::SignatureHelpParams params;
        params.path = src.path;
        params.version = 1;
        params.text = text;
        params.directory = "/tmp";
        params.arguments = make_args(src.path);
        params.offset = 45;  // after "foo(" (26 + 19)

        auto result = co_await w.peer->send_request(params);
        EXPECT_TRUE(result.has_value());
        // Should return signature help for foo(int a, int b).
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

TEST_CASE(MultipleStatelessRequests) {
    std::vector<std::unique_ptr<TempFile>> files;
    for(int i = 0; i < 3; i++) {
        auto text = "int idx_var_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
        files.push_back(
            std::make_unique<TempFile>("multi_index_" + std::to_string(i) + ".cpp", text));
    }

    WorkerHandle w;
    ASSERT_TRUE(w.spawn("stateless-worker"));

    bool test_done = false;

    w.run([&]() -> et::task<> {
        // Send multiple index requests to test stateless worker handles them sequentially.
        for(int i = 0; i < 3; i++) {
            worker::IndexParams params;
            params.file = files[i]->path;
            params.directory = "/tmp";
            params.arguments = make_args(files[i]->path);

            auto result = co_await w.peer->send_request(params);
            EXPECT_TRUE(result.has_value());
        }
        test_done = true;
        w.peer->close_output();
    });

    ASSERT_TRUE(test_done);
}

};  // TEST_SUITE(StatelessWorkerExtended)

}  // namespace

}  // namespace clice::testing
