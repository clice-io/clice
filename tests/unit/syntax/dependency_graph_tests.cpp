#include "test/temp_dir.h"
#include "test/test.h"
#include "command/command.h"
#include "support/path_pool.h"
#include "syntax/dependency_graph.h"

namespace clice::testing {
namespace {

TEST_SUITE(DependencyGraph) {

// ============================================================================
// Module mapping tests
// ============================================================================

TEST_CASE(LookupModuleEmpty) {
    clice::DependencyGraph graph;
    EXPECT_TRUE(graph.lookup_module("foo.bar").empty());
}

TEST_CASE(AddAndLookupModule) {
    clice::DependencyGraph graph;
    graph.add_module("foo.bar", 42);

    auto result = graph.lookup_module("foo.bar");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 42u);
}

TEST_CASE(DuplicateModuleDedup) {
    clice::DependencyGraph graph;
    // Same module name, same path_id — should dedup.
    graph.add_module("foo", 10);
    graph.add_module("foo", 10);
    ASSERT_EQ(graph.lookup_module("foo").size(), 1u);

    // Same module name, different path_id — multiple candidates.
    graph.add_module("foo", 20);
    auto result = graph.lookup_module("foo");
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], 10u);
    EXPECT_EQ(result[1], 20u);
}

TEST_CASE(MultipleModules) {
    clice::DependencyGraph graph;
    graph.add_module("mod.a", 1);
    graph.add_module("mod.b", 2);
    graph.add_module("mod.c:part", 3);

    ASSERT_EQ(graph.lookup_module("mod.a").size(), 1u);
    EXPECT_EQ(graph.lookup_module("mod.a")[0], 1u);
    ASSERT_EQ(graph.lookup_module("mod.b").size(), 1u);
    EXPECT_EQ(graph.lookup_module("mod.b")[0], 2u);
    ASSERT_EQ(graph.lookup_module("mod.c:part").size(), 1u);
    EXPECT_EQ(graph.lookup_module("mod.c:part")[0], 3u);
    EXPECT_TRUE(graph.lookup_module("mod.d").empty());
}

TEST_CASE(ModuleCount) {
    clice::DependencyGraph graph;
    EXPECT_EQ(graph.module_count(), 0u);

    graph.add_module("a", 1);
    EXPECT_EQ(graph.module_count(), 1u);

    graph.add_module("b", 2);
    EXPECT_EQ(graph.module_count(), 2u);

    // Second candidate for "a" doesn't increase module name count.
    graph.add_module("a", 3);
    EXPECT_EQ(graph.module_count(), 2u);
}

// ============================================================================
// Include edge tests
// ============================================================================

TEST_CASE(EmptyGraphIncludes) {
    clice::DependencyGraph graph;
    auto includes = graph.get_includes(0, 0);
    EXPECT_TRUE(includes.empty());
}

TEST_CASE(SetAndGetIncludes) {
    clice::DependencyGraph graph;
    llvm::SmallVector<std::uint32_t> ids = {10, 20, 30};
    graph.set_includes(1, 0, ids);

    auto result = graph.get_includes(1, 0);
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 10u);
    EXPECT_EQ(result[1], 20u);
    EXPECT_EQ(result[2], 30u);
}

TEST_CASE(IncludesPerConfig) {
    clice::DependencyGraph graph;

    // Same file, different configs.
    graph.set_includes(1, 0, {10, 20});
    graph.set_includes(1, 1, {20, 30});

    auto config0 = graph.get_includes(1, 0);
    ASSERT_EQ(config0.size(), 2u);
    EXPECT_EQ(config0[0], 10u);
    EXPECT_EQ(config0[1], 20u);

    auto config1 = graph.get_includes(1, 1);
    ASSERT_EQ(config1.size(), 2u);
    EXPECT_EQ(config1[0], 20u);
    EXPECT_EQ(config1[1], 30u);
}

TEST_CASE(GetAllIncludesUnion) {
    clice::DependencyGraph graph;

    graph.set_includes(1, 0, {10, 20});
    graph.set_includes(1, 1, {20, 30});

    auto all = graph.get_all_includes(1);
    // Union of {10, 20} and {20, 30} = {10, 20, 30}.
    ASSERT_EQ(all.size(), 3u);
}

TEST_CASE(ConditionalFlag) {
    clice::DependencyGraph graph;

    constexpr auto FLAG = clice::DependencyGraph::CONDITIONAL_FLAG;
    constexpr auto MASK = clice::DependencyGraph::PATH_ID_MASK;

    // PathID 5 unconditional, PathID 7 conditional.
    llvm::SmallVector<std::uint32_t> ids = {5, 7 | FLAG};
    graph.set_includes(1, 0, ids);

    auto result = graph.get_includes(1, 0);
    ASSERT_EQ(result.size(), 2u);

    // First: unconditional.
    EXPECT_EQ(result[0] & MASK, 5u);
    EXPECT_EQ(result[0] & FLAG, 0u);

    // Second: conditional.
    EXPECT_EQ(result[1] & MASK, 7u);
    EXPECT_NE(result[1] & FLAG, 0u);
}

TEST_CASE(FileCount) {
    clice::DependencyGraph graph;
    EXPECT_EQ(graph.file_count(), 0u);

    graph.set_includes(1, 0, {10});
    EXPECT_EQ(graph.file_count(), 1u);

    // Same file, different config.
    graph.set_includes(1, 1, {20});
    EXPECT_EQ(graph.file_count(), 1u);

    // Different file.
    graph.set_includes(2, 0, {30});
    EXPECT_EQ(graph.file_count(), 2u);
}

TEST_CASE(EdgeCount) {
    clice::DependencyGraph graph;
    EXPECT_EQ(graph.edge_count(), 0u);

    graph.set_includes(1, 0, {10, 20});
    EXPECT_EQ(graph.edge_count(), 2u);

    graph.set_includes(2, 0, {30});
    EXPECT_EQ(graph.edge_count(), 3u);
}

TEST_CASE(EmptyIncludes) {
    clice::DependencyGraph graph;
    graph.set_includes(1, 0, {});

    auto result = graph.get_includes(1, 0);
    EXPECT_TRUE(result.empty());
    EXPECT_EQ(graph.file_count(), 1u);
    EXPECT_EQ(graph.edge_count(), 0u);
}

};  // TEST_SUITE(DependencyGraph)

// ============================================================================
// scan_dependency_graph() integration tests
// ============================================================================

/// Write a compile_commands.json into the temp dir and load it into the given CDB.
void write_cdb(TempDir& tmp, CompilationDatabase& cdb, llvm::StringRef json_content) {
    tmp.touch("compile_commands.json", json_content);
    cdb.load(tmp.path("compile_commands.json"));
}

/// Helper: build a compile_commands.json array from entries.
/// Uses "arguments" array form to avoid platform-specific tokenization issues
/// (e.g. TokenizeGNUCommandLine treating backslashes as escape characters).
struct CDBEntry {
    llvm::StringRef dir;
    std::string file;
    std::vector<std::string> extra_args;
};

/// Escape backslashes and quotes for JSON string values.
std::string json_escape(llvm::StringRef s) {
    std::string result;
    result.reserve(s.size());
    for(char c: s) {
        if(c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    return result;
}

std::string build_cdb_json(llvm::ArrayRef<CDBEntry> entries) {
    std::string json = "[\n";
    for(std::size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if(i > 0) {
            json += ",\n";
        }
        json += R"(  {"directory": ")";
        json += json_escape(e.dir);
        json += R"(", "file": ")";
        json += json_escape(e.file);
        json += R"(", "arguments": ["clang++", "-std=c++20")";
        for(auto& arg: e.extra_args) {
            json += R"(, ")";
            json += json_escape(arg);
            json += R"(")";
        }
        json += R"(, ")";
        json += json_escape(e.file);
        json += R"("]})";
    }
    json += "\n]";
    return json;
}

TEST_SUITE(ScanDependencyGraph) {

TEST_CASE(EmptyCDB) {
    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    scan_dependency_graph(cdb, pool, graph);

    EXPECT_EQ(graph.file_count(), 0u);
    EXPECT_EQ(graph.module_count(), 0u);
    EXPECT_EQ(graph.edge_count(), 0u);
}

TEST_CASE(SingleFileNoIncludes) {
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(int main() { return 0; })");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_EQ(graph.file_count(), 1u);
    EXPECT_EQ(graph.edge_count(), 0u);
    EXPECT_EQ(graph.module_count(), 0u);
}

TEST_CASE(SingleFileWithInclude) {
    TempDir tmp;
    tmp.touch("include/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
int main() { return x; }
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("include")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_GE(graph.file_count(), 1u);
    EXPECT_GE(graph.edge_count(), 1u);
}

TEST_CASE(TransitiveIncludes) {
    TempDir tmp;
    tmp.touch("inc/a.h", R"(#include "b.h")");
    tmp.touch("inc/b.h", R"(#include "c.h")");
    tmp.touch("inc/c.h", R"(int c = 3;)");
    tmp.touch("src/main.cpp", R"(
#include "a.h"
int main() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    // main->a, a->b, b->c across 4 waves.
    EXPECT_GE(graph.file_count(), 3u);
    EXPECT_GE(graph.edge_count(), 3u);
}

TEST_CASE(MultipleSourceFiles) {
    TempDir tmp;
    tmp.touch("inc/shared.h", R"(int shared = 1;)");
    tmp.touch("src/a.cpp", R"(
#include "shared.h"
void a() {}
)");
    tmp.touch("src/b.cpp", R"(
#include "shared.h"
void b() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    std::vector<std::string> inc = {"-I", tmp.path("inc")};
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/a.cpp"), inc},
        {tmp.root, tmp.path("src/b.cpp"), inc},
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_GE(graph.file_count(), 2u);
    EXPECT_GE(graph.edge_count(), 2u);
}

TEST_CASE(ConditionalIncludes) {
    TempDir tmp;
    tmp.touch("inc/always.h", R"(// always)");
    tmp.touch("inc/maybe.h", R"(// maybe)");
    tmp.touch("src/main.cpp", R"(
#include "always.h"
#ifdef FOO
#include "maybe.h"
#endif
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    // Both headers discovered (over-approximate).
    EXPECT_GE(graph.edge_count(), 2u);

    // Verify conditional flag.
    bool found_unconditional = false;
    bool found_conditional = false;
    auto includes = graph.get_includes(pool.cache[tmp.path("src/main.cpp")], 0);
    for(auto id: includes) {
        if(id & DependencyGraph::CONDITIONAL_FLAG) {
            found_conditional = true;
        } else {
            found_unconditional = true;
        }
    }
    EXPECT_TRUE(found_unconditional);
    EXPECT_TRUE(found_conditional);
}

TEST_CASE(ModuleExtraction) {
    TempDir tmp;
    tmp.touch("src/mymod.cpp", R"(
export module my.module;
export int foo() { return 42; }
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mymod.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    auto result = graph.lookup_module("my.module");
    ASSERT_EQ(result.size(), 1u);

    auto path = pool.resolve(result[0]);
    EXPECT_TRUE(llvm::sys::fs::equivalent(path, tmp.path("src/mymod.cpp")));
}

TEST_CASE(ModulePartition) {
    TempDir tmp;
    tmp.touch("src/mod.cpp", R"(
export module my.mod:part;
void impl() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mod.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    ASSERT_EQ(graph.lookup_module("my.mod:part").size(), 1u);
}

TEST_CASE(DiamondIncludes) {
    TempDir tmp;
    tmp.touch("inc/common.h", R"(int common = 1;)");
    tmp.touch("inc/a.h", R"(
#include "common.h"
int a = 1;
)");
    tmp.touch("inc/b.h", R"(
#include "common.h"
int b = 1;
)");
    tmp.touch("src/main.cpp", R"(
#include "a.h"
#include "b.h"
int main() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    // main->a, main->b, a->common, b->common.
    EXPECT_GE(graph.edge_count(), 4u);
    EXPECT_GE(graph.file_count(), 3u);
}

TEST_CASE(AngledVsQuoted) {
    TempDir tmp;
    tmp.touch("quoted/header.h", R"(int q = 1;)");
    tmp.touch("angled/header.h", R"(int a = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
#include <header.h>
int main() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root,
         tmp.path("src/main.cpp"),
         {"-iquote", tmp.path("quoted"), "-I", tmp.path("angled")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_GE(graph.edge_count(), 2u);
}

TEST_CASE(MissingInclude) {
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(
#include "nonexistent.h"
int main() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_EQ(graph.file_count(), 1u);
    EXPECT_EQ(graph.edge_count(), 0u);
}

TEST_CASE(MultipleModules) {
    TempDir tmp;
    tmp.touch("src/mod_a.cpp", R"(
export module mod.a;
void a() {}
)");
    tmp.touch("src/mod_b.cpp", R"(
export module mod.b;
void b() {}
)");
    tmp.touch("src/impl.cpp", R"(
module mod.a;
void a_impl() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mod_a.cpp"), {}},
        {tmp.root, tmp.path("src/mod_b.cpp"), {}},
        {tmp.root, tmp.path("src/impl.cpp"),  {}},
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    EXPECT_EQ(graph.module_count(), 2u);
    ASSERT_FALSE(graph.lookup_module("mod.a").empty());
    ASSERT_FALSE(graph.lookup_module("mod.b").empty());
}

TEST_CASE(DeepIncludeChain) {
    TempDir tmp;
    tmp.touch("inc/h4.h", R"(int h4 = 4;)");
    tmp.touch("inc/h3.h", R"(#include "h4.h")");
    tmp.touch("inc/h2.h", R"(#include "h3.h")");
    tmp.touch("inc/h1.h", R"(#include "h2.h")");
    tmp.touch("inc/h0.h", R"(#include "h1.h")");
    tmp.touch("src/main.cpp", R"(
#include "h0.h"
int main() {}
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    // main->h0->h1->h2->h3->h4 across 5 waves.
    EXPECT_GE(graph.edge_count(), 5u);
    EXPECT_GE(graph.file_count(), 5u);
}

TEST_CASE(ModuleWithIncludes) {
    TempDir tmp;
    tmp.touch("inc/util.h", R"(int util = 1;)");
    tmp.touch("src/mymod.cpp", R"(
module;
#include "util.h"
export module my.lib;
export int value() { return util; }
)");

    CompilationDatabase cdb;
    PathPool pool;
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mymod.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_dependency_graph(cdb, pool, graph);

    ASSERT_FALSE(graph.lookup_module("my.lib").empty());
    EXPECT_GE(graph.edge_count(), 1u);
}

};  // TEST_SUITE(ScanDependencyGraph)

}  // namespace
}  // namespace clice::testing
