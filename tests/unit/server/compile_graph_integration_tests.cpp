#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/command.h"
#include "compile/compilation.h"
#include "server/compile_graph.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"
#include "syntax/scan.h"

namespace clice::testing {
namespace {

namespace et = eventide;

void write_cdb(TempDir& tmp, CompilationDatabase& cdb, llvm::StringRef json_content) {
    tmp.touch("compile_commands.json", json_content);
    cdb.load(tmp.path("compile_commands.json"));
}

/// Build a dispatch_fn that compiles PCMs in-process (no workers).
/// Clang requires ALL transitive PCM deps (not just direct imports)
/// in PrebuiltModuleFiles, so we pass every available PCM.
CompileGraph::dispatch_fn make_dispatch(CompilationDatabase& cdb,
                                        PathPool& pool,
                                        DependencyGraph& graph,
                                        llvm::DenseMap<std::uint32_t, std::string>& pcm_paths) {
    return [&](std::uint32_t path_id) -> et::task<bool> {
        auto file_path = pool.resolve(path_id);
        auto results = cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty()) {
            co_return false;
        }

        CompilationParams cp;
        cp.kind = CompilationKind::ModuleInterface;
        cp.directory = results[0].directory.str();
        for(auto* arg: results[0].arguments) {
            cp.arguments.push_back(arg);
        }

        // Fill ALL available PCM paths (clang needs transitive deps too).
        for(auto& [pid, pcm_path]: pcm_paths) {
            for(auto& [mod_name, mod_ids]: graph.modules()) {
                if(llvm::find(mod_ids, pid) != mod_ids.end()) {
                    cp.pcms.try_emplace(mod_name, pcm_path);
                    break;
                }
            }
        }

        auto tmp = fs::createTemporaryFile("test-pcm", "pcm");
        if(!tmp) {
            co_return false;
        }
        cp.output_file = *tmp;

        PCMInfo info;
        auto unit = compile(cp, info);

        if(unit.completed()) {
            pcm_paths[path_id] = std::string(cp.output_file);
            co_return true;
        }
        co_return false;
    };
}

/// Build a resolve_fn that lazily scans module files for imports.
CompileGraph::resolve_fn make_resolver(CompilationDatabase& cdb,
                                       PathPool& pool,
                                       DependencyGraph& graph) {
    return [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        auto file_path = pool.resolve(path_id);
        auto results = cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty()) {
            return {};
        }

        auto scan_result = scan_precise(results[0].arguments, results[0].directory);

        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }
        return deps;
    };
}

/// Helper to set up infra, compile a module, and verify all PCMs are produced.
struct ModuleTestEnv {
    TempDir tmp;
    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;
    llvm::DenseMap<std::uint32_t, std::string> pcm_paths;

    void setup(llvm::ArrayRef<CDBEntry> entries, llvm::StringRef json) {
        write_cdb(tmp, cdb, json);
        scan_dependency_graph(cdb, pool, graph);
    }

    std::uint32_t lookup(llvm::StringRef mod_name) {
        auto ids = graph.lookup_module(mod_name);
        return ids.empty() ? UINT32_MAX : ids[0];
    }
};

TEST_SUITE(CompileGraphIntegration) {

// ============================================================================
// Basic module interface units
// ============================================================================

TEST_CASE(SingleModuleNoDeps) {
    ModuleTestEnv env;
    env.tmp.touch("mod_a.cppm", "export module A;\n" "export int foo() { return 42; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("mod_a.cppm"), {}}
    });
    env.setup({}, json);

    ASSERT_FALSE(env.graph.lookup_module("A").empty());
    auto pid_a = env.lookup("A");

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_a]() -> et::task<> {
        auto result = co_await cg.compile(pid_a).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_TRUE(env.pcm_paths.contains(pid_a));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(ChainedModules) {
    ModuleTestEnv env;
    env.tmp.touch("mod_a.cppm", "export module A;\n" "export int foo() { return 42; }\n");
    env.tmp.touch("mod_b.cppm",
                  "export module B;\n"
                  "import A;\n"
                  "export int bar() { return foo() + 1; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("mod_a.cppm"), {}},
        {env.tmp.root, env.tmp.path("mod_b.cppm"), {}},
    });
    env.setup({}, json);

    auto pid_a = env.lookup("A");
    auto pid_b = env.lookup("B");
    ASSERT_NE(pid_a, UINT32_MAX);
    ASSERT_NE(pid_b, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_a, pid_b]() -> et::task<> {
        auto result = co_await cg.compile(pid_b).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_TRUE(env.pcm_paths.contains(pid_a));
        EXPECT_TRUE(env.pcm_paths.contains(pid_b));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

TEST_CASE(DiamondModules) {
    ModuleTestEnv env;
    env.tmp.touch("mod_base.cppm",
                  "export module Base;\n" "export int base_val() { return 10; }\n");
    env.tmp.touch("mod_left.cppm",
                  "export module Left;\n"
                  "import Base;\n"
                  "export int left_val() { return base_val() + 1; }\n");
    env.tmp.touch("mod_right.cppm",
                  "export module Right;\n"
                  "import Base;\n"
                  "export int right_val() { return base_val() + 2; }\n");
    env.tmp.touch("mod_top.cppm",
                  "export module Top;\n"
                  "import Left;\n"
                  "import Right;\n"
                  "export int top_val() { return left_val() + right_val(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("mod_base.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_left.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_right.cppm"), {}},
        {env.tmp.root, env.tmp.path("mod_top.cppm"),   {}},
    });
    env.setup({}, json);

    auto pid_top = env.lookup("Top");
    ASSERT_NE(pid_top, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_top]() -> et::task<> {
        auto result = co_await cg.compile(pid_top).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 4u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Dotted module names
// ============================================================================

TEST_CASE(DottedModuleName) {
    ModuleTestEnv env;
    env.tmp.touch("io.cppm", "export module my.io;\n" "export void print() {}\n");
    env.tmp.touch("app.cppm",
                  "export module my.app;\n"
                  "import my.io;\n"
                  "export void run() { print(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("io.cppm"),  {}},
        {env.tmp.root, env.tmp.path("app.cppm"), {}},
    });
    env.setup({}, json);

    auto pid_app = env.lookup("my.app");
    ASSERT_NE(pid_app, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_app]() -> et::task<> {
        auto result = co_await cg.compile(pid_app).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Re-export (export import)
// ============================================================================

TEST_CASE(ReExport) {
    ModuleTestEnv env;
    env.tmp.touch("core.cppm", "export module Core;\n" "export int core_fn() { return 1; }\n");
    env.tmp.touch("wrapper.cppm",
                  "export module Wrapper;\n"
                  "export import Core;\n"
                  "export int wrap_fn() { return core_fn() + 10; }\n");
    env.tmp.touch("user.cppm",
                  "export module User;\n"
                  "import Wrapper;\n"
                  "// core_fn() is accessible via re-export.\n"
                  "export int use_fn() { return core_fn() + wrap_fn(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("core.cppm"),    {}},
        {env.tmp.root, env.tmp.path("wrapper.cppm"), {}},
        {env.tmp.root, env.tmp.path("user.cppm"),    {}},
    });
    env.setup({}, json);

    auto pid_user = env.lookup("User");
    ASSERT_NE(pid_user, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_user]() -> et::task<> {
        auto result = co_await cg.compile(pid_user).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 3u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Export block syntax
// ============================================================================

TEST_CASE(ExportBlock) {
    ModuleTestEnv env;
    env.tmp.touch("block.cppm",
                  "export module Block;\n"
                  "export {\n"
                  "    int alpha() { return 1; }\n"
                  "    int beta() { return 2; }\n"
                  "    namespace ns {\n"
                  "        int gamma() { return 3; }\n"
                  "    }\n"
                  "}\n");
    env.tmp.touch("consumer.cppm",
                  "export module Consumer;\n"
                  "import Block;\n"
                  "export int total() { return alpha() + beta() + ns::gamma(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("block.cppm"),    {}},
        {env.tmp.root, env.tmp.path("consumer.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("Consumer");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Global module fragment
// ============================================================================

TEST_CASE(GlobalModuleFragment) {
    ModuleTestEnv env;
    env.tmp.touch("legacy.h", "inline int legacy_fn() { return 99; }\n");
    env.tmp.touch("gmf.cppm",
                  "module;\n"
                  R"(#include "legacy.h")" "\n"
                  "export module GMF;\n"
                  "export int wrapped() { return legacy_fn(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("gmf.cppm"), {"-I", env.tmp.path(".")}},
    });
    env.setup({}, json);

    auto pid = env.lookup("GMF");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_TRUE(env.pcm_paths.contains(pid));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Private module fragment
// ============================================================================

TEST_CASE(PrivateModuleFragment) {
    ModuleTestEnv env;
    env.tmp.touch("priv.cppm",
                  "export module Priv;\n"
                  "export int public_fn();\n"
                  "module : private;\n"
                  "int public_fn() { return 42; }\n"
                  "int private_helper() { return 7; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("priv.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("Priv");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_TRUE(env.pcm_paths.contains(pid));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Module partitions — interface partition
// ============================================================================

TEST_CASE(PartitionInterface) {
    ModuleTestEnv env;
    // Partition interface unit.
    env.tmp.touch("part.cppm", "export module M:Part;\n" "export int part_fn() { return 5; }\n");
    // Primary module interface re-exports the partition.
    env.tmp.touch("primary.cppm",
                  "export module M;\n"
                  "export import :Part;\n"
                  "export int primary_fn() { return part_fn() + 1; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("part.cppm"),    {}},
        {env.tmp.root, env.tmp.path("primary.cppm"), {}},
    });
    env.setup({}, json);

    // The partition is registered as "M:Part", primary as "M".
    auto pid_m = env.lookup("M");
    ASSERT_NE(pid_m, UINT32_MAX);
    ASSERT_NE(env.lookup("M:Part"), UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_m]() -> et::task<> {
        auto result = co_await cg.compile(pid_m).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Both partition and primary should be compiled.
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Multiple partitions
// ============================================================================

TEST_CASE(MultiplePartitions) {
    ModuleTestEnv env;
    env.tmp.touch("part_a.cppm", "export module Lib:A;\n" "export int a_fn() { return 1; }\n");
    env.tmp.touch("part_b.cppm", "export module Lib:B;\n" "export int b_fn() { return 2; }\n");
    env.tmp.touch("lib.cppm",
                  "export module Lib;\n"
                  "export import :A;\n"
                  "export import :B;\n"
                  "export int lib_fn() { return a_fn() + b_fn(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("part_a.cppm"), {}},
        {env.tmp.root, env.tmp.path("part_b.cppm"), {}},
        {env.tmp.root, env.tmp.path("lib.cppm"),    {}},
    });
    env.setup({}, json);

    auto pid_lib = env.lookup("Lib");
    ASSERT_NE(pid_lib, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_lib]() -> et::task<> {
        auto result = co_await cg.compile(pid_lib).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Lib:A, Lib:B, and Lib.
        EXPECT_EQ(env.pcm_paths.size(), 3u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Partition importing another partition (within same module)
// ============================================================================

TEST_CASE(PartitionChain) {
    ModuleTestEnv env;
    env.tmp.touch("types.cppm",
                  "export module Sys:Types;\n" "export struct Config { int value = 0; };\n");
    env.tmp.touch("core.cppm",
                  "export module Sys:Core;\n"
                  "import :Types;\n"
                  "export Config make_config() { return {42}; }\n");
    env.tmp.touch("sys.cppm",
                  "export module Sys;\n"
                  "export import :Types;\n"
                  "export import :Core;\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("types.cppm"), {}},
        {env.tmp.root, env.tmp.path("core.cppm"),  {}},
        {env.tmp.root, env.tmp.path("sys.cppm"),   {}},
    });
    env.setup({}, json);

    auto pid_sys = env.lookup("Sys");
    ASSERT_NE(pid_sys, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_sys]() -> et::task<> {
        auto result = co_await cg.compile(pid_sys).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Sys:Types, Sys:Core, Sys.
        EXPECT_EQ(env.pcm_paths.size(), 3u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Module with exported namespace
// ============================================================================

TEST_CASE(ExportNamespace) {
    ModuleTestEnv env;
    env.tmp.touch("ns.cppm",
                  "export module NS;\n"
                  "export namespace math {\n"
                  "    int add(int a, int b) { return a + b; }\n"
                  "    int mul(int a, int b) { return a * b; }\n"
                  "}\n");
    env.tmp.touch("calc.cppm",
                  "export module Calc;\n"
                  "import NS;\n"
                  "export int compute() { return math::add(3, math::mul(4, 5)); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("ns.cppm"),   {}},
        {env.tmp.root, env.tmp.path("calc.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("Calc");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// GMF with include + module import
// ============================================================================

TEST_CASE(GMFWithImport) {
    ModuleTestEnv env;
    env.tmp.touch("util.h", "inline int util_helper() { return 7; }\n");
    env.tmp.touch("base.cppm", "export module Base;\n" "export int base() { return 100; }\n");
    env.tmp.touch("combined.cppm",
                  "module;\n"
                  R"(#include "util.h")" "\n"
                  "export module Combined;\n"
                  "import Base;\n"
                  "export int combined() { return base() + util_helper(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("base.cppm"),     {}                       },
        {env.tmp.root, env.tmp.path("combined.cppm"), {"-I", env.tmp.path(".")}},
    });
    env.setup({}, json);

    auto pid = env.lookup("Combined");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Deep chain (5 modules)
// ============================================================================

TEST_CASE(DeepChain) {
    ModuleTestEnv env;
    env.tmp.touch("m1.cppm", "export module M1;\n" "export int f1() { return 1; }\n");
    env.tmp.touch("m2.cppm",
                  "export module M2;\n"
                  "import M1;\n"
                  "export int f2() { return f1() + 1; }\n");
    env.tmp.touch("m3.cppm",
                  "export module M3;\n"
                  "import M2;\n"
                  "export int f3() { return f2() + 1; }\n");
    env.tmp.touch("m4.cppm",
                  "export module M4;\n"
                  "import M3;\n"
                  "export int f4() { return f3() + 1; }\n");
    env.tmp.touch("m5.cppm",
                  "export module M5;\n"
                  "import M4;\n"
                  "export int f5() { return f4() + 1; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("m1.cppm"), {}},
        {env.tmp.root, env.tmp.path("m2.cppm"), {}},
        {env.tmp.root, env.tmp.path("m3.cppm"), {}},
        {env.tmp.root, env.tmp.path("m4.cppm"), {}},
        {env.tmp.root, env.tmp.path("m5.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("M5");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 5u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Multiple independent modules (no shared deps)
// ============================================================================

TEST_CASE(IndependentModules) {
    ModuleTestEnv env;
    env.tmp.touch("x.cppm", "export module X;\n" "export int x() { return 1; }\n");
    env.tmp.touch("y.cppm", "export module Y;\n" "export int y() { return 2; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("x.cppm"), {}},
        {env.tmp.root, env.tmp.path("y.cppm"), {}},
    });
    env.setup({}, json);

    auto pid_x = env.lookup("X");
    auto pid_y = env.lookup("Y");
    ASSERT_NE(pid_x, UINT32_MAX);
    ASSERT_NE(pid_y, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_x, pid_y]() -> et::task<> {
        auto r1 = co_await cg.compile(pid_x).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        auto r2 = co_await cg.compile(pid_y).catch_cancel();
        EXPECT_TRUE(r2.has_value() && *r2);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Module with template exports
// ============================================================================

TEST_CASE(TemplateExport) {
    ModuleTestEnv env;
    env.tmp.touch("tmpl.cppm",
                  "export module Tmpl;\n"
                  "export template<typename T>\n"
                  "T identity(T x) { return x; }\n"
                  "export template<typename T, typename U>\n"
                  "auto pair_sum(T a, U b) { return a + b; }\n");
    env.tmp.touch("use_tmpl.cppm",
                  "export module UseTmpl;\n"
                  "import Tmpl;\n"
                  "export int test() { return identity(42) + pair_sum(1, 2); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("tmpl.cppm"),     {}},
        {env.tmp.root, env.tmp.path("use_tmpl.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("UseTmpl");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Module with class export and inheritance across modules
// ============================================================================

TEST_CASE(ClassExportAndInheritance) {
    ModuleTestEnv env;
    env.tmp.touch("shape.cppm",
                  "export module Shape;\n"
                  "export class Shape {\n"
                  "public:\n"
                  "    virtual ~Shape() = default;\n"
                  "    virtual int area() const = 0;\n"
                  "};\n");
    env.tmp.touch("circle.cppm",
                  "export module Circle;\n"
                  "import Shape;\n"
                  "export class Circle : public Shape {\n"
                  "    int r;\n"
                  "public:\n"
                  "    Circle(int r) : r(r) {}\n"
                  "    int area() const override { return 3 * r * r; }\n"
                  "};\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("shape.cppm"),  {}},
        {env.tmp.root, env.tmp.path("circle.cppm"), {}},
    });
    env.setup({}, json);

    auto pid = env.lookup("Circle");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Recompile after update (invalidation + recompile)
// ============================================================================

TEST_CASE(RecompileAfterUpdate) {
    ModuleTestEnv env;
    env.tmp.touch("leaf.cppm", "export module Leaf;\n" "export int leaf() { return 1; }\n");
    env.tmp.touch("mid.cppm",
                  "export module Mid;\n"
                  "import Leaf;\n"
                  "export int mid() { return leaf() + 1; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("leaf.cppm"), {}},
        {env.tmp.root, env.tmp.path("mid.cppm"),  {}},
    });
    env.setup({}, json);

    auto pid_leaf = env.lookup("Leaf");
    auto pid_mid = env.lookup("Mid");
    ASSERT_NE(pid_leaf, UINT32_MAX);
    ASSERT_NE(pid_mid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_leaf, pid_mid]() -> et::task<> {
        // First compile.
        auto r1 = co_await cg.compile(pid_mid).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
        EXPECT_FALSE(cg.is_dirty(pid_leaf));
        EXPECT_FALSE(cg.is_dirty(pid_mid));

        // Simulate editing Leaf — should cascade to Mid.
        cg.update(pid_leaf);
        EXPECT_TRUE(cg.is_dirty(pid_leaf));
        EXPECT_TRUE(cg.is_dirty(pid_mid));

        // Recompile.
        auto r2 = co_await cg.compile(pid_mid).catch_cancel();
        EXPECT_TRUE(r2.has_value() && *r2);
        EXPECT_FALSE(cg.is_dirty(pid_leaf));
        EXPECT_FALSE(cg.is_dirty(pid_mid));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Partition with GMF (#include inside global module fragment of partition)
// ============================================================================

TEST_CASE(PartitionWithGMF) {
    ModuleTestEnv env;
    env.tmp.touch("config.h", "#define MAX_SIZE 100\n");
    env.tmp.touch("part_cfg.cppm",
                  "module;\n"
                  R"(#include "config.h")" "\n"
                  "export module Cfg:Limits;\n"
                  "export constexpr int max_size = MAX_SIZE;\n");
    env.tmp.touch("cfg.cppm", "export module Cfg;\n" "export import :Limits;\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("part_cfg.cppm"), {"-I", env.tmp.path(".")}},
        {env.tmp.root, env.tmp.path("cfg.cppm"),      {}                       },
    });
    env.setup({}, json);

    auto pid = env.lookup("Cfg");
    ASSERT_NE(pid, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid]() -> et::task<> {
        auto result = co_await cg.compile(pid).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        EXPECT_EQ(env.pcm_paths.size(), 2u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Cross-module partition + external import
// ============================================================================

TEST_CASE(PartitionWithExternalImport) {
    ModuleTestEnv env;
    // External module.
    env.tmp.touch("ext.cppm", "export module Ext;\n" "export int ext_val() { return 99; }\n");
    // Partition that imports the external module.
    env.tmp.touch("part.cppm",
                  "export module App:Core;\n"
                  "import Ext;\n"
                  "export int core_fn() { return ext_val() + 1; }\n");
    // Primary module interface.
    env.tmp.touch("app.cppm", "export module App;\n" "export import :Core;\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("ext.cppm"),  {}},
        {env.tmp.root, env.tmp.path("part.cppm"), {}},
        {env.tmp.root, env.tmp.path("app.cppm"),  {}},
    });
    env.setup({}, json);

    auto pid_app = env.lookup("App");
    ASSERT_NE(pid_app, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_app]() -> et::task<> {
        auto result = co_await cg.compile(pid_app).catch_cancel();
        EXPECT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
        // Ext, App:Core, App.
        EXPECT_EQ(env.pcm_paths.size(), 3u);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Diamond update cascade + recompile
// ============================================================================

TEST_CASE(DiamondUpdateCascade) {
    ModuleTestEnv env;
    env.tmp.touch("mod_base.cppm",
                  "export module Base;\n" "export int base_val() { return 10; }\n");
    env.tmp.touch("mod_left.cppm",
                  "export module Left;\n"
                  "import Base;\n"
                  "export int left_val() { return base_val() + 1; }\n");
    env.tmp.touch("mod_right.cppm",
                  "export module Right;\n"
                  "import Base;\n"
                  "export int right_val() { return base_val() + 2; }\n");
    env.tmp.touch("mod_top.cppm",
                  "export module Top;\n"
                  "import Left;\n"
                  "import Right;\n"
                  "export int top_val() { return left_val() + right_val(); }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("mod_base.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_left.cppm"),  {}},
        {env.tmp.root, env.tmp.path("mod_right.cppm"), {}},
        {env.tmp.root, env.tmp.path("mod_top.cppm"),   {}},
    });
    env.setup({}, json);

    auto pid_base = env.lookup("Base");
    auto pid_left = env.lookup("Left");
    auto pid_right = env.lookup("Right");
    auto pid_top = env.lookup("Top");
    ASSERT_NE(pid_base, UINT32_MAX);
    ASSERT_NE(pid_top, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_base, pid_left, pid_right, pid_top]() -> et::task<> {
        // Initial compile.
        auto r1 = co_await cg.compile(pid_top).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        EXPECT_EQ(env.pcm_paths.size(), 4u);

        // Save old PCM paths.
        auto old_base_pcm = env.pcm_paths[pid_base];

        // Update base: should cascade to Left, Right, Top.
        auto dirtied = cg.update(pid_base);
        EXPECT_TRUE(cg.is_dirty(pid_base));
        EXPECT_TRUE(cg.is_dirty(pid_left));
        EXPECT_TRUE(cg.is_dirty(pid_right));
        EXPECT_TRUE(cg.is_dirty(pid_top));

        // Simulate MasterServer: erase stale PCMs for all dirtied nodes.
        for(auto id: dirtied) {
            env.pcm_paths.erase(id);
        }
        EXPECT_EQ(env.pcm_paths.size(), 0u);

        // Recompile.
        auto r2 = co_await cg.compile(pid_top).catch_cancel();
        EXPECT_TRUE(r2.has_value() && *r2);
        EXPECT_EQ(env.pcm_paths.size(), 4u);
        // PCM path should have changed (new temp file).
        EXPECT_NE(env.pcm_paths[pid_base], old_base_pcm);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Verify resolve_fn is re-invoked after update (resolved=false)
// ============================================================================

TEST_CASE(ReResolveAfterUpdate) {
    ModuleTestEnv env;
    // Start with Mid importing Leaf.
    env.tmp.touch("leaf.cppm", "export module Leaf;\n" "export int leaf() { return 1; }\n");
    env.tmp.touch("extra.cppm", "export module Extra;\n" "export int extra() { return 99; }\n");
    env.tmp.touch("mid.cppm",
                  "export module Mid;\n"
                  "import Leaf;\n"
                  "export int mid() { return leaf() + 1; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("leaf.cppm"),  {}},
        {env.tmp.root, env.tmp.path("extra.cppm"), {}},
        {env.tmp.root, env.tmp.path("mid.cppm"),   {}},
    });
    env.setup({}, json);

    auto pid_leaf = env.lookup("Leaf");
    auto pid_extra = env.lookup("Extra");
    auto pid_mid = env.lookup("Mid");
    ASSERT_NE(pid_mid, UINT32_MAX);
    ASSERT_NE(pid_extra, UINT32_MAX);

    int resolve_count = 0;
    auto counting_resolver = [&](std::uint32_t path_id) -> llvm::SmallVector<std::uint32_t> {
        if(path_id == pid_mid) {
            resolve_count++;
        }
        // Delegate to the standard resolver.
        auto file_path = env.pool.resolve(path_id);
        auto results =
            env.cdb.lookup(file_path, {.query_toolchain = true, .suppress_logging = true});
        if(results.empty()) {
            return {};
        }
        auto scan_result = scan_precise(results[0].arguments, results[0].directory);
        llvm::SmallVector<std::uint32_t> deps;
        for(auto& mod_name: scan_result.modules) {
            auto mod_ids = env.graph.lookup_module(mod_name);
            if(!mod_ids.empty()) {
                deps.push_back(mod_ids[0]);
            }
        }
        return deps;
    };

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    std::move(counting_resolver));

    et::event_loop loop;
    auto test = [this, &cg, &env, &resolve_count, pid_mid]() -> et::task<> {
        // First compile: resolve_fn called once for Mid.
        auto r1 = co_await cg.compile(pid_mid).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        EXPECT_EQ(resolve_count, 1);

        // Update Mid: resets resolved.
        cg.update(pid_mid);

        // Recompile: resolve_fn should be called again.
        auto r2 = co_await cg.compile(pid_mid).catch_cancel();
        EXPECT_TRUE(r2.has_value() && *r2);
        EXPECT_EQ(resolve_count, 2);
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Compilation failure propagation (real clang error)
// ============================================================================

TEST_CASE(CompileFailurePropagation) {
    ModuleTestEnv env;
    // Good module.
    env.tmp.touch("good.cppm", "export module Good;\n" "export int good() { return 1; }\n");
    // Bad module with syntax error.
    env.tmp.touch("bad.cppm",
                  "export module Bad;\n"
                  "import Good;\n"
                  "export int bad() { return UNDEFINED_SYMBOL; }\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("good.cppm"), {}},
        {env.tmp.root, env.tmp.path("bad.cppm"),  {}},
    });
    env.setup({}, json);

    auto pid_bad = env.lookup("Bad");
    ASSERT_NE(pid_bad, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_bad]() -> et::task<> {
        auto result = co_await cg.compile(pid_bad).catch_cancel();
        EXPECT_TRUE(result.has_value());
        // Compilation should fail due to undefined symbol.
        EXPECT_FALSE(*result);
        // Good module should still have been compiled successfully.
        auto pid_good = env.lookup("Good");
        EXPECT_TRUE(env.pcm_paths.contains(pid_good));
        // Bad module should NOT have a PCM.
        EXPECT_FALSE(env.pcm_paths.contains(pid_bad));
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

// ============================================================================
// Module implementation unit (consumes PCM, doesn't produce one)
// ============================================================================

TEST_CASE(ModuleImplementationUnit) {
    ModuleTestEnv env;
    // Module interface unit — produces PCM.
    env.tmp.touch("iface.cppm", "export module Greeter;\n" "export const char* greet();\n");
    // Module implementation unit — consumes PCM, no export.
    env.tmp.touch("impl.cpp",
                  "module Greeter;\n" R"(const char* greet() { return "hello"; })" "\n");

    auto json = build_cdb_json({
        {env.tmp.root, env.tmp.path("iface.cppm"), {}},
        {env.tmp.root, env.tmp.path("impl.cpp"),   {}},
    });
    env.setup({}, json);

    auto pid_iface = env.lookup("Greeter");
    ASSERT_NE(pid_iface, UINT32_MAX);

    CompileGraph cg(make_dispatch(env.cdb, env.pool, env.graph, env.pcm_paths),
                    make_resolver(env.cdb, env.pool, env.graph));

    et::event_loop loop;
    auto test = [this, &cg, &env, pid_iface]() -> et::task<> {
        // Build the interface PCM via CompileGraph.
        auto r1 = co_await cg.compile(pid_iface).catch_cancel();
        EXPECT_TRUE(r1.has_value() && *r1);
        EXPECT_TRUE(env.pcm_paths.contains(pid_iface));

        // Now compile the implementation unit as Content (like a stateful worker would).
        auto impl_path = env.tmp.path("impl.cpp");
        auto results =
            env.cdb.lookup(impl_path, {.query_toolchain = true, .suppress_logging = true});
        CO_ASSERT_FALSE(results.empty());

        CompilationParams cp;
        cp.kind = CompilationKind::Content;
        cp.directory = results[0].directory.str();
        for(auto* arg: results[0].arguments) {
            cp.arguments.push_back(arg);
        }
        // Pass the built PCM so clang can resolve `module Greeter;`.
        for(auto& [pid, pcm_path]: env.pcm_paths) {
            for(auto& [mod_name, mod_ids]: env.graph.modules()) {
                if(llvm::find(mod_ids, pid) != mod_ids.end()) {
                    cp.pcms.try_emplace(mod_name, pcm_path);
                    break;
                }
            }
        }

        auto unit = compile(cp);
        EXPECT_TRUE(unit.completed());
    };
    auto t = test();
    loop.schedule(t);
    loop.run();
}

};  // TEST_SUITE(CompileGraphIntegration)

}  // namespace
}  // namespace clice::testing
