#include <string>
#include <vector>

#include "test/test.h"
#include "server/protocol.h"
#include "server/worker_test_helpers.h"
#include "syntax/scan.h"

namespace clice::testing {

namespace {

namespace et = eventide;

// ============================================================================
// End-to-end PCH compilation through real workers:
//   1. Stateless worker builds PCH for preamble headers
//   2. Stateful worker compiles a file using the PCH
// ============================================================================

TEST_SUITE(PCHWorker) {

TEST_CASE(BuildPCHThenCompile) {
    TempDir tmp;

    tmp.touch("common.h", R"cpp(struct Point { int x, y; };)cpp" "\n");
    auto header = tmp.path("common.h");

    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_file = tmp.path("main.cpp");

    auto dir = std::string(tmp.root);

    // --- Phase 1: Build PCH via stateless worker ---
    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn("stateless-worker"));

    std::string pch_path;
    bool phase1_done = false;

    sl.run([&]() -> et::task<> {
        worker::BuildPCHParams params;
        params.file = main_file;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-x",
                            "c++-header",
                            "-I",
                            dir,
                            main_file};
        params.content = main_text;

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        CO_ASSERT_TRUE(result.value().success);
        pch_path = result.value().pch_path;
        EXPECT_FALSE(pch_path.empty());

        phase1_done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(phase1_done);
    ASSERT_FALSE(pch_path.empty());

    // Verify the PCH file exists on disk.
    ASSERT_TRUE(llvm::sys::fs::exists(pch_path));

    // --- Phase 2: Compile with PCH via stateful worker ---
    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn("stateful-worker"));

    bool phase2_done = false;

    auto preamble_bound = compute_preamble_bound(main_text);

    sf.run([&]() -> et::task<> {
        worker::CompileParams params;
        params.path = main_file;
        params.version = 1;
        params.text = main_text;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-fsyntax-only",
                            "-I",
                            dir,
                            main_file};
        params.pch = {pch_path, preamble_bound};

        auto result = co_await sf.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);

        phase2_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(phase2_done);

    // Cleanup PCH temp file.
    std::remove(pch_path.c_str());
}

TEST_CASE(CompileWithoutPCHStillWorks) {
    TempDir tmp;

    tmp.touch("common.h", R"cpp(struct Point { int x, y; };)cpp" "\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_file = tmp.path("main.cpp");

    auto dir = std::string(tmp.root);

    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn("stateful-worker"));

    bool compile_done = false;

    sf.run([&]() -> et::task<> {
        worker::CompileParams params;
        params.path = main_file;
        params.version = 1;
        params.text = main_text;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-fsyntax-only",
                            "-I",
                            dir,
                            main_file};
        // pch left as default (empty path, 0 bound).

        auto result = co_await sf.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);

        compile_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(compile_done);
}

};  // TEST_SUITE(PCHWorker)

}  // namespace
}  // namespace clice::testing
