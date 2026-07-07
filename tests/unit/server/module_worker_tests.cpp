#include <string>
#include <vector>

#include "test/test.h"
#include "index/tu_index.h"
#include "server/protocol/worker.h"
#include "server/worker_test_helpers.h"

#include "llvm/ADT/STLExtras.h"

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

    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn());

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

    WorkerHandle sf;
    ASSERT_TRUE(sf.spawn(4ULL * 1024 * 1024 * 1024));

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

        // The imported module's interface source is a dependency of the
        // consumer, hashed from its on-disk content, with the compile start
        // time alongside for the mtime fast layer.
        auto& deps = result.value().deps;
        auto dep = llvm::find_if(deps, [&](const HashedDep& dep) { return dep.path == iface; });
        CO_ASSERT_TRUE(dep != deps.end());
        EXPECT_TRUE(dep->hash != 0);
        EXPECT_TRUE(result.value().build_at > 0);

        phase2_done = true;
        sf.peer->close_output();
    });

    ASSERT_TRUE(phase2_done);

    // Cleanup PCM temp file.
    std::remove(pcm_path.c_str());
}

TEST_CASE(IndexImporterReportsImports) {
    TempDir tmp;
    tmp.touch("mod_iface.cppm",
              "export module Hello;\n" R"(export inline int hello() { return 1; })" "\n");
    auto iface = tmp.path("mod_iface.cppm");
    tmp.touch("consumer.cpp", "import Hello;\n" "int use_hello() { return hello(); }\n");
    auto consumer = tmp.path("consumer.cpp");

    WorkerHandle sl;
    ASSERT_TRUE(sl.spawn());

    bool done = false;

    sl.run([&]() -> kota::task<> {
        std::string pcm_path;
        {
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
            CO_ASSERT_TRUE(result.has_value() && result.value().success);
            pcm_path = result.value().output_path;

            // Build results carry the compile start time and the interface
            // source with its consumed-content hash.
            EXPECT_TRUE(result.value().build_at > 0);
            auto& deps = result.value().deps;
            auto dep = llvm::find_if(deps, [&](const HashedDep& dep) { return dep.path == iface; });
            CO_ASSERT_TRUE(dep != deps.end());
            EXPECT_TRUE(dep->hash != 0);
        }

        // Background-index the consumer against the PCM: the TUIndex must
        // carry the interface source as an import dependency with a hash.
        worker::BuildParams params;
        params.kind = worker::BuildKind::Index;
        params.file = consumer;
        params.directory = "/tmp";
        params.arguments = {"clang++",
                            "-resource-dir",
                            std::string(resource_dir()),
                            "-std=c++20",
                            "-fsyntax-only",
                            consumer};
        params.pcms = {
            {"Hello", pcm_path}
        };

        auto result = co_await sl.peer->send_request(params);
        CO_ASSERT_TRUE(result.has_value());
        CO_ASSERT_TRUE(result.value().success);
        CO_ASSERT_FALSE(result.value().tu_index_data.empty());

        auto tu_index = index::TUIndex::from(result.value().tu_index_data.data());
        CO_ASSERT_EQ(tu_index.imports.size(), 1u);
        auto import_id = tu_index.imports[0];
        EXPECT_EQ(tu_index.graph.paths[import_id], iface);
        EXPECT_TRUE(tu_index.graph.path_hashes[import_id] != 0);

        std::remove(pcm_path.c_str());
        done = true;
        sl.peer->close_output();
    });

    ASSERT_TRUE(done);
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
    ASSERT_TRUE(sl.spawn());

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
    ASSERT_TRUE(sf.spawn(4ULL * 1024 * 1024 * 1024));

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

        // The consumer spells only `import B;`, but loading B's PCM pulls
        // A's too — both interface sources must be reported as deps.
        auto has_dep = [&](llvm::StringRef path) {
            return llvm::any_of(result.value().deps,
                                [&](const HashedDep& dep) { return dep.path == path; });
        };
        EXPECT_TRUE(has_dep(mod_a));
        EXPECT_TRUE(has_dep(mod_b));

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
    ASSERT_TRUE(sl.spawn());

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
    ASSERT_TRUE(sf.spawn(4ULL * 1024 * 1024 * 1024));

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
