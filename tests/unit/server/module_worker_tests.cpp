#include <string>
#include <vector>

#include "test/test.h"
#include "server/protocol/worker.h"
#include "server/worker_test_helpers.h"

namespace clice::testing {

namespace {

// ============================================================================
// End-to-end module compilation through real workers:
//   1. Stateless worker builds PCM for module interface
//   2. Stateful worker compiles a file that imports the module using the PCM
// This tests the same pipeline as MasterServer.run_build_drain().
// ============================================================================

TEST_SUITE(ModuleWorker) {

TEST_CASE(BuildPCMThenCompileWithImport) {
    TempDir tmp;
    // Module interface: produces PCM.
    tmp.touch("mod_iface.cppm",
              "export module Hello;\n" R"(export const char* hello() { return "world"; })" "\n");
    auto iface = tmp.path("mod_iface.cppm");

    // Consumer: imports the module.
    tmp.touch("consumer.cpp", "import Hello;\n" "int main() { return hello()[0]; }\n");
    auto consumer = tmp.path("consumer.cpp");

    // --- Phase 1: Build PCM via stateless worker ---
    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn("stateless-worker"));

    std::string pcm_path;
    bool phase1_done = false;

    sl.run([&]() -> kota::task<> {
        worker::BuildParams params;
        params.kind = worker::BuildKind::BuildPCM;
        params.file = iface;
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "--precompile",
                            iface};
        params.module_name = "Hello";
        params.output_path = tmp.path("Hello.pcm");

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        CO_ASSERT_TRUE(result.value().success);
        pcm_path = result.value().output_path;
        EXPECT_FALSE(pcm_path.empty());

        phase1_done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(phase1_done);
    ASSERT_FALSE(pcm_path.empty());

    // --- Phase 2: Compile consumer with the PCM via stateful worker ---
    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn("stateful-worker"));

    bool phase2_done = false;

    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = consumer;
        params.version = 1;
        params.text = "import Hello;\n" "int main() { return hello()[0]; }\n";
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "-fsyntax-only",
                            consumer};
        // Pass the PCM — same as MasterServer fills CompileParams.pcms.
        params.pcms = {
            {"Hello", pcm_path}
        };

        auto result = co_await sf.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);

        phase2_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(phase2_done);

    // Cleanup PCM temp file.
    std::remove(pcm_path.c_str());
}

TEST_CASE(BuildPCMChainThenCompile) {
    TempDir tmp;
    // Module A: no deps.
    tmp.touch("chain_a.cppm", "export module A;\n" "export int val_a() { return 1; }\n");
    auto mod_a = tmp.path("chain_a.cppm");
    // Module B: imports A.
    tmp.touch("chain_b.cppm",
              "export module B;\n"
              "import A;\n"
              "export int val_b() { return val_a() + 1; }\n");
    auto mod_b = tmp.path("chain_b.cppm");
    // Consumer: imports B (transitively needs A).
    tmp.touch("chain_consumer.cpp", "import B;\n" "int main() { return val_b(); }\n");
    auto consumer = tmp.path("chain_consumer.cpp");

    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn("stateless-worker"));

    std::string pcm_a, pcm_b;
    bool pcm_done = false;

    sl.run([&]() -> kota::task<> {
        // Build PCM for A first.
        {
            worker::BuildParams params;
            params.kind = worker::BuildKind::BuildPCM;
            params.file = mod_a;
            params.directory = "/tmp";
            params.arguments = {"clang++",
                                "-resource-dir",
                                std::string(resource_dir()),
                                "-std=c++20",
                                "--precompile",
                                mod_a};
            params.module_name = "A";
            params.output_path = tmp.path("A.pcm");

            auto result = co_await sl.peer->send_request(params);
            CO_ASSERT_TRUE(result.has_value() && result.value().success);
            pcm_a = result.value().output_path;
        }

        // Build PCM for B, passing A's PCM (transitive dep).
        {
            worker::BuildParams params;
            params.kind = worker::BuildKind::BuildPCM;
            params.file = mod_b;
            params.directory = "/tmp";
            params.arguments = {"clang++",
                                "-resource-dir",
                                std::string(resource_dir()),
                                "-std=c++20",
                                "--precompile",
                                mod_b};
            params.module_name = "B";
            params.output_path = tmp.path("B.pcm");
            params.pcms = {
                {"A", pcm_a}
            };

            auto result = co_await sl.peer->send_request(params);
            CO_ASSERT_TRUE(result.has_value() && result.value().success);
            pcm_b = result.value().output_path;
        }

        pcm_done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(pcm_done);

    // Compile consumer with BOTH PCMs via stateful worker.
    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn("stateful-worker"));

    bool compile_done = false;

    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = consumer;
        params.version = 1;
        params.text = "import B;\n" "int main() { return val_b(); }\n";
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "-fsyntax-only",
                            consumer};
        // Clang needs ALL transitive PCMs.
        params.pcms = {
            {"A", pcm_a},
            {"B", pcm_b}
        };

        auto result = co_await sf.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);

        compile_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(compile_done);

    std::remove(pcm_a.c_str());
    std::remove(pcm_b.c_str());
}

TEST_CASE(ModuleImplementationUnitWithWorker) {
    TempDir tmp;
    // Module interface.
    tmp.touch("impl_iface.cppm", "export module Calc;\n" "export int add(int a, int b);\n");
    auto iface = tmp.path("impl_iface.cppm");
    // Module implementation unit (no export).
    tmp.touch("impl_unit.cpp", "module Calc;\n" "int add(int a, int b) { return a + b; }\n");
    auto impl = tmp.path("impl_unit.cpp");

    // Build PCM for interface.
    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn("stateless-worker"));

    std::string pcm_path;
    bool pcm_done = false;

    sl.run([&]() -> kota::task<> {
        worker::BuildParams params;
        params.kind = worker::BuildKind::BuildPCM;
        params.file = iface;
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "--precompile",
                            iface};
        params.module_name = "Calc";
        params.output_path = tmp.path("Calc.pcm");

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value() && result.value().success);
        pcm_path = result.value().output_path;

        pcm_done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(pcm_done);

    // Compile implementation unit with the PCM via stateful worker.
    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn("stateful-worker"));

    bool compile_done = false;

    sf.run([&]() -> kota::task<> {
        worker::CompileParams params;
        params.path = impl;
        params.version = 1;
        params.text = "module Calc;\n" "int add(int a, int b) { return a + b; }\n";
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "-fsyntax-only",
                            impl};
        params.pcms = {
            {"Calc", pcm_path}
        };

        auto result = co_await sf.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value().version, 1);

        compile_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(compile_done);

    std::remove(pcm_path.c_str());
}

};  // TEST_SUITE(ModuleWorker)

}  // namespace
}  // namespace clice::testing
