module;

#include "support/prelude.h"

module clice;

import :project.project;
import :syntax.preamble;
import :unittests.server.worker_test_helpers;
import :unittests.utils;
import :worker.protocol;

namespace clice::testing {

namespace {

// ============================================================================
// End-to-end PCH compilation through real workers:
//   1. Stateless worker builds PCH for preamble headers
//   2. Stateful worker compiles a file using the PCH
// ============================================================================

ZEST_SUITE(PCHWorker) {

ZEST_CASE(BuildPCHThenCompile) {
    TempDir tmp;

    tmp.touch("common.h",
              R"cpp(struct Point { int x, y; };)cpp"
              "\n");
    auto header = tmp.path("common.h");

    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_path = tmp.path("main.cpp");

    auto dir = std::string(tmp.root);

    WorkerHandle sl;
    ZASSERT(sl.spawn());

    std::string pch_path;
    bool phase1_done = false;

    sl.run([&]() -> kota::task<> {
        worker::BuildPCHParams params;
        params.file = main_path;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-x",
                            "c++-header",
                            "-I",
                            dir,
                            main_path};
        params.content = main_text;
        params.preamble_bound = preamble_bound(main_text);
        params.output_path = tmp.path("preamble.pch");
        params.index_output_path = tmp.path("preamble.pch.idx");

        auto result = co_await sl.peer->send_request(params);
        ZASSERT(result);
        ZASSERT(result.value().success);
        pch_path = result.value().output_path;
        ZEXPECT(!pch_path.empty());

        phase1_done = true;
        sl.peer->close_output();
    });

    ZASSERT(phase1_done);
    ZASSERT(!pch_path.empty());

    // Verify the PCH file exists on disk.
    ZASSERT(fs::exists(pch_path));

    // The worker wrote the paired preamble index: it must load and
    // carry the preamble's document links (the #include of common.h).
    auto state = load_pch_index(tmp.path("preamble.pch.idx"));
    ZASSERT(state != nullptr);
    bool has_common_link = ranges::any_of(state->links(), [&](auto& link) {
        return llvm::StringRef(link.target).ends_with("common.h");
    });
    ZEXPECT(has_common_link);

    WorkerHandle sf;
    ZASSERT(sf.spawn(true));

    bool phase2_done = false;

    auto bound = preamble_bound(main_text);

    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = main_path;
        params.version = 1;
        params.text = main_text;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-fsyntax-only",
                            "-I",
                            dir,
                            main_path};
        params.pch = {pch_path, bound};

        auto result = co_await sf.peer->send_request(params);
        ZASSERT(result);
        ZEXPECT(result.value().version == 1);

        phase2_done = true;
        sf.peer->close_output();
    });

    ZASSERT(phase2_done);
}

ZEST_CASE(UserFatalErrorSparesPCH) {
    TempDir tmp;
    tmp.touch("common.h", "struct Point { int x, y; };\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1, 2};\nimport missing;\n";
    tmp.touch("main.cpp", main_text);
    auto main_path = tmp.path("main.cpp");
    auto dir = std::string(tmp.root);
    auto bound = preamble_bound(main_text);
    std::vector<std::string> arguments = {"clang++",
                                          "-resource-dir",
                                          std::string(resource_dir()),
                                          "-std=c++20",
                                          "-I",
                                          dir,
                                          main_path};

    WorkerHandle sl;
    ZASSERT(sl.spawn());
    std::string pch_path;
    sl.run([&]() -> kota::task<> {
        worker::BuildPCHParams params;
        params.file = main_path;
        params.directory = dir;
        params.arguments = arguments;
        params.content = main_text;
        params.preamble_bound = bound;
        params.output_path = tmp.path("preamble.pch");
        params.index_output_path = tmp.path("preamble.pch.idx");

        auto result = co_await sl.peer->send_request(params);
        ZASSERT(result);
        ZASSERT(result.value().success);
        pch_path = result.value().output_path;
        sl.peer->close_output();
    });
    ZASSERT(!pch_path.empty());

    // The missing module is fatal and the parse goes on past it: the
    // compile is a result, and its error names no precompiled file.
    WorkerHandle sf;
    ZASSERT(sf.spawn(true));
    bool done = false;
    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = main_path;
        params.version = 1;
        params.text = main_text;
        params.directory = dir;
        params.arguments = arguments;
        params.pch = {pch_path, bound};

        auto result = co_await sf.peer->send_request(params);
        ZASSERT(result);
        ZEXPECT(result.value().status == worker::CompileStatus::Done);
        ZEXPECT(!result.value().pch_suspect);
        ZEXPECT(ranges::any_of(result.value().diagnostics, [](auto& diagnostic) {
            return llvm::StringRef(diagnostic.message).contains("module 'missing' not found");
        }));
        done = true;
        sf.peer->close_output();
    });
    ZASSERT(done);
}

ZEST_CASE(BlobWriteFailure) {
    TempDir tmp;

    tmp.touch("common.h",
              R"cpp(struct Point { int x, y; };)cpp"
              "\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_path = tmp.path("main.cpp");
    auto dir = std::string(tmp.root);

    WorkerHandle sl;
    ZASSERT(sl.spawn());

    bool done = false;
    sl.run([&]() -> kota::task<> {
        worker::BuildPCHParams params;
        params.file = main_path;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-x",
                            "c++-header",
                            "-I",
                            dir,
                            main_path};
        params.content = main_text;
        params.preamble_bound = preamble_bound(main_text);
        params.output_path = tmp.path("preamble.pch");
        // Unwritable blob path: the whole build must fail (the PCH is only
        // served together with its blob), classified as an internal error.
        params.index_output_path = tmp.path("no_such_dir/preamble.pch.idx");

        auto result = co_await sl.peer->send_request(params);
        ZASSERT(result);
        ZEXPECT(!result.value().success);
        ZEXPECT(!result.value().has_user_errors);

        done = true;
        sl.peer->close_output();
    });

    ZASSERT(done);
}

ZEST_CASE(CompileWithoutPCHStillWorks) {
    TempDir tmp;

    tmp.touch("common.h",
              R"cpp(struct Point { int x, y; };)cpp"
              "\n");
    std::string main_text = "#include \"common.h\"\nPoint p{1,2};\n";
    tmp.touch("main.cpp", main_text);
    auto main_path = tmp.path("main.cpp");

    auto dir = std::string(tmp.root);

    WorkerHandle sf;
    ZASSERT(sf.spawn(true));

    bool compile_done = false;

    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = main_path;
        params.version = 1;
        params.text = main_text;
        params.directory = dir;
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-fsyntax-only",
                            "-I",
                            dir,
                            main_path};
        // pch left as default (empty path, 0 bound).

        auto result = co_await sf.peer->send_request(params);
        ZASSERT(result);
        ZEXPECT(result.value().version == 1);

        compile_done = true;
        sf.peer->close_output();
    });

    ZASSERT(compile_done);
}

};  // ZEST_SUITE(PCHWorker)

}  // namespace
}  // namespace clice::testing
