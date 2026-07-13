#include <algorithm>
#include <string>
#include <vector>

#include "test/test.h"
#include "index/preamble_state.h"
#include "server/protocol/worker.h"
#include "server/worker_test_helpers.h"
#include "syntax/scan.h"

namespace clice::testing {

namespace {

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

    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn());

    std::string pch_path;
    bool phase1_done = false;

    sl.run([&]() -> kota::task<> {
        worker::BuildParams params;
        params.kind = worker::BuildKind::BuildPCH;
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
        params.text = main_text;
        params.preamble_bound = compute_preamble_bound(main_text);
        params.output_path = tmp.path("preamble.pch");
        params.index_output_path = tmp.path("preamble.pch.idx");

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        CO_ASSERT_TRUE(result.value().success);
        pch_path = result.value().output_path;
        EXPECT_FALSE(pch_path.empty());

        phase1_done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(phase1_done);
    ASSERT_FALSE(pch_path.empty());

    // Verify the PCH file exists on disk.
    ASSERT_TRUE(llvm::sys::fs::exists(pch_path));

    // The worker wrote the paired PreambleState blob: it must load and
    // carry the preamble's document links (the #include of common.h).
    auto state = index::PreambleState::load(tmp.path("preamble.pch.idx"));
    ASSERT_TRUE(state != nullptr);
    bool has_common_link = std::ranges::any_of(state->links(), [&](auto& link) {
        return llvm::StringRef(link.target).ends_with("common.h");
    });
    EXPECT_TRUE(has_common_link);

    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn(4ULL * 1024 * 1024 * 1024));

    bool phase2_done = false;

    auto preamble_bound = compute_preamble_bound(main_text);

    sf.run([&]() -> kota::task<> {
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

TEST_CASE(BlobWriteFailure) {
    TempDir tmp;

    tmp.touch("common.h", R"cpp(struct Point { int x, y; };)cpp" "\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_file = tmp.path("main.cpp");
    auto dir = std::string(tmp.root);

    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn());

    bool done = false;
    sl.run([&]() -> kota::task<> {
        worker::BuildParams params;
        params.kind = worker::BuildKind::BuildPCH;
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
        params.text = main_text;
        params.preamble_bound = compute_preamble_bound(main_text);
        params.output_path = tmp.path("preamble.pch");
        // Unwritable blob path: the whole build must fail (the PCH is only
        // served together with its blob), classified as an internal error.
        params.index_output_path = tmp.path("no_such_dir/preamble.pch.idx");

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result.value().success);
        EXPECT_FALSE(result.value().has_user_errors);

        done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(done);
}

TEST_CASE(CompileWithoutPCHStillWorks) {
    TempDir tmp;

    tmp.touch("common.h", R"cpp(struct Point { int x, y; };)cpp" "\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_file = tmp.path("main.cpp");

    auto dir = std::string(tmp.root);

    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn(4ULL * 1024 * 1024 * 1024));

    bool compile_done = false;

    sf.run([&]() -> kota::task<> {
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
