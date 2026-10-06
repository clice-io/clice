#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "command/command.h"
#include "syntax/dependency_graph.h"
#include "vfs/file_table.h"

#include "llvm/ADT/STLExtras.h"

namespace clice::testing {
namespace {

ZEST_SUITE(DependencyGraph) {

// ============================================================================
// Module mapping tests
// ============================================================================

ZEST_CASE(LookupModuleEmpty) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.lookup_module("foo.bar").empty());
}

ZEST_CASE(AddAndLookupModule) {
    clice::DependencyGraph graph;
    graph.add_module("foo.bar", Fid{42});

    auto result = graph.lookup_module("foo.bar");
    ZASSERT(result.size() == 1u);
    ZEXPECT(result[0].raw == 42u);
}

ZEST_CASE(DuplicateModuleDedup) {
    clice::DependencyGraph graph;
    // Same module name, same path_id — should dedup.
    graph.add_module("foo", Fid{10});
    graph.add_module("foo", Fid{10});
    ZASSERT(graph.lookup_module("foo").size() == 1u);

    // Same module name, different path_id — multiple candidates.
    graph.add_module("foo", Fid{20});
    auto result = graph.lookup_module("foo");
    ZASSERT(result.size() == 2u);
    ZEXPECT(result[0].raw == 10u);
    ZEXPECT(result[1].raw == 20u);
}

ZEST_CASE(RedeclareKeepsProviderOrder) {
    clice::DependencyGraph graph;
    graph.update_module_decl(Fid{1}, "foo");
    graph.update_module_decl(Fid{2}, "foo");

    // Providers are selected by list order: re-declaring the unchanged
    // name must not rotate the duplicate-name list.
    graph.update_module_decl(Fid{1}, "foo");
    auto result = graph.lookup_module("foo");
    ZASSERT(result.size() == 2u);
    ZEXPECT(result[0].raw == 1u);
    ZEXPECT(result[1].raw == 2u);

    // A real name change still moves the path.
    graph.update_module_decl(Fid{1}, "bar");
    ZASSERT(graph.lookup_module("foo").size() == 1u);
    ZEXPECT(graph.lookup_module("foo")[0].raw == 2u);
    ZASSERT(graph.lookup_module("bar").size() == 1u);
}

ZEST_CASE(ModuleOfFollowsDeclarations) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.module_of(Fid{1}).empty());

    graph.add_module("foo", Fid{1});
    ZEXPECT(graph.module_of(Fid{1}) == "foo");

    // A re-declaration moves the file: the old name loses it, the
    // reverse view follows in the same write.
    graph.update_module_decl(Fid{1}, "bar");
    ZEXPECT(graph.module_of(Fid{1}) == "bar");
    ZEXPECT(graph.lookup_module("foo").empty());

    // Dropping the declaration leaves the name behind as an empty
    // provider list, yet the file declares nothing.
    graph.update_module_decl(Fid{1}, {});
    ZEXPECT(graph.module_of(Fid{1}).empty());
    ZEXPECT(graph.lookup_module("bar").empty());
}

ZEST_CASE(ModuleOfFollowsRedeclaredName) {
    // A file scanned under two configurations can be listed under two
    // names (a macro picks the declaration). Re-declaring one of them
    // keeps both provider lists untouched — no reselection — but the
    // file's own declaration is the one the save met.
    clice::DependencyGraph graph;
    graph.add_module("a", Fid{1});
    graph.add_module("b", Fid{1});
    ZEXPECT(graph.module_of(Fid{1}) == "b");

    graph.update_module_decl(Fid{1}, "a");
    ZEXPECT(graph.module_of(Fid{1}) == "a");
    ZEXPECT(llvm::is_contained(graph.lookup_module("a"), Fid{1}));
    ZEXPECT(llvm::is_contained(graph.lookup_module("b"), Fid{1}));
}

ZEST_CASE(MultipleModules) {
    clice::DependencyGraph graph;
    graph.add_module("mod.a", Fid{1});
    graph.add_module("mod.b", Fid{2});
    graph.add_module("mod.c:part", Fid{3});

    ZASSERT(graph.lookup_module("mod.a").size() == 1u);
    ZEXPECT(graph.lookup_module("mod.a")[0].raw == 1u);
    ZASSERT(graph.lookup_module("mod.b").size() == 1u);
    ZEXPECT(graph.lookup_module("mod.b")[0].raw == 2u);
    ZASSERT(graph.lookup_module("mod.c:part").size() == 1u);
    ZEXPECT(graph.lookup_module("mod.c:part")[0].raw == 3u);
    ZEXPECT(graph.lookup_module("mod.d").empty());
}

ZEST_CASE(ModuleCount) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.module_count() == 0u);

    graph.add_module("a", Fid{1});
    ZEXPECT(graph.module_count() == 1u);

    graph.add_module("b", Fid{2});
    ZEXPECT(graph.module_count() == 2u);

    // Second candidate for "a" doesn't increase module name count.
    graph.add_module("a", Fid{3});
    ZEXPECT(graph.module_count() == 2u);
}

// ============================================================================
// Include edge tests
// ============================================================================

ZEST_CASE(EmptyGraphIncludes) {
    clice::DependencyGraph graph;
    auto includes = graph.get_includes(Fid{0}, 0);
    ZEXPECT(includes.empty());
}

ZEST_CASE(SetAndGetIncludes) {
    clice::DependencyGraph graph;
    llvm::SmallVector<IncludeEdge> ids = {{Fid{10}}, {Fid{20}}, {Fid{30}}};
    graph.set_includes(Fid{1}, 0, ids);

    auto result = graph.get_includes(Fid{1}, 0);
    ZASSERT(result.size() == 3u);
    ZEXPECT(result[0].fid.raw == 10u);
    ZEXPECT(result[1].fid.raw == 20u);
    ZEXPECT(result[2].fid.raw == 30u);
}

ZEST_CASE(IncludesPerConfig) {
    clice::DependencyGraph graph;

    // Same file, different configs.
    graph.set_includes(Fid{1}, 0, {{Fid{10}}, {Fid{20}}});
    graph.set_includes(Fid{1}, 1, {{Fid{20}}, {Fid{30}}});

    auto config0 = graph.get_includes(Fid{1}, 0);
    ZASSERT(config0.size() == 2u);
    ZEXPECT(config0[0].fid.raw == 10u);
    ZEXPECT(config0[1].fid.raw == 20u);

    auto config1 = graph.get_includes(Fid{1}, 1);
    ZASSERT(config1.size() == 2u);
    ZEXPECT(config1[0].fid.raw == 20u);
    ZEXPECT(config1[1].fid.raw == 30u);
}

ZEST_CASE(GetAllIncludesUnion) {
    clice::DependencyGraph graph;

    graph.set_includes(Fid{1}, 0, {{Fid{10}}, {Fid{20}}});
    graph.set_includes(Fid{1}, 1, {{Fid{20}}, {Fid{30}}});

    auto all = graph.get_all_includes(Fid{1});
    // Union of {10, 20} and {20, 30} = {10, 20, 30}.
    ZASSERT(all.size() == 3u);
    ZEXPECT(llvm::is_contained(all, Fid{10}));
    ZEXPECT(llvm::is_contained(all, Fid{20}));
    ZEXPECT(llvm::is_contained(all, Fid{30}));
}

ZEST_CASE(ConditionalFlag) {
    clice::DependencyGraph graph;

    // PathID 5 unconditional, PathID 7 conditional.
    llvm::SmallVector<IncludeEdge> ids = {
        {Fid{5}},
        {Fid{7}, /*conditional=*/true}
    };
    graph.set_includes(Fid{1}, 0, ids);

    auto result = graph.get_includes(Fid{1}, 0);
    ZASSERT(result.size() == 2u);

    // First: unconditional.
    ZEXPECT(result[0].fid.raw == 5u);
    ZEXPECT(!result[0].conditional);

    // Second: conditional.
    ZEXPECT(result[1].fid.raw == 7u);
    ZEXPECT(result[1].conditional);
}

ZEST_CASE(FileCount) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.file_count() == 0u);

    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    ZEXPECT(graph.file_count() == 1u);

    // Same file, different config.
    graph.set_includes(Fid{1}, 1, {{Fid{20}}});
    ZEXPECT(graph.file_count() == 1u);

    // Different file.
    graph.set_includes(Fid{2}, 0, {{Fid{30}}});
    ZEXPECT(graph.file_count() == 2u);
}

ZEST_CASE(EdgeCount) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.edge_count() == 0u);

    graph.set_includes(Fid{1}, 0, {{Fid{10}}, {Fid{20}}});
    ZEXPECT(graph.edge_count() == 2u);

    graph.set_includes(Fid{2}, 0, {{Fid{30}}});
    ZEXPECT(graph.edge_count() == 3u);
}

ZEST_CASE(EmptyIncludes) {
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {});

    auto result = graph.get_includes(Fid{1}, 0);
    ZEXPECT(result.empty());
    ZEXPECT(graph.file_count() == 1u);
    ZEXPECT(graph.edge_count() == 0u);
}

ZEST_CASE(IncluderListedOnce) {
    // One includer through two configurations is one includer.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    graph.set_includes(Fid{1}, 1, {{Fid{10}}});
    graph.set_includes(Fid{2}, 0, {{Fid{10}}});
    graph.build_reverse_map();
    ZASSERT(graph.get_includers(Fid{10}) == (llvm::ArrayRef<Fid>{Fid{1}, Fid{2}}));
}

ZEST_CASE(ClearDropsEveryConfig) {
    // Clearing a file drops every configuration's edges, and nothing of
    // other files.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    graph.set_includes(Fid{1}, 1, {{Fid{20}}});
    graph.set_includes(Fid{2}, 0, {{Fid{10}}});

    graph.clear_includes(Fid{1});
    ZASSERT(graph.get_all_includes(Fid{1}).empty());
    ZASSERT(graph.get_all_includes(Fid{2}) == llvm::SmallVector<Fid>{Fid{10}});
}

ZEST_CASE(EdgesKeepReverseMap) {
    // Once built, the reverse map follows every edge update.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    graph.build_reverse_map();

    graph.set_includes(Fid{2}, 0, {{Fid{10}}, {Fid{20}}});
    ZASSERT(graph.get_includers(Fid{10}) == (llvm::ArrayRef<Fid>{Fid{1}, Fid{2}}));
    ZASSERT(graph.get_includers(Fid{20}) == llvm::ArrayRef<Fid>{Fid{2}});

    // A second configuration keeps an edge the first one drops.
    graph.set_includes(Fid{2}, 1, {{Fid{20}}});
    graph.set_includes(Fid{2}, 0, {{Fid{10}}});
    ZASSERT(graph.get_includers(Fid{20}) == llvm::ArrayRef<Fid>{Fid{2}});

    graph.clear_includes(Fid{2});
    ZASSERT(graph.get_includers(Fid{10}) == llvm::ArrayRef<Fid>{Fid{1}});
    ZASSERT(graph.get_includers(Fid{20}).empty());
}

ZEST_CASE(ReadersClimbForcedIncludes) {
    // Unit 1 includes header 10; units 1 and 2 force header 20 in, which
    // includes 30. Hosting stops at the forced header, readers do not.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    graph.set_includes(Fid{20}, 0, {{Fid{30}}});
    graph.add_forced_include(Fid{2}, Fid{20});
    graph.add_forced_include(Fid{1}, Fid{20});
    graph.build_reverse_map();

    ZASSERT(graph.get_forcing_units(Fid{20}) == (llvm::ArrayRef<Fid>{Fid{1}, Fid{2}}));
    ZASSERT(graph.get_includers(Fid{20}).empty());
    ZASSERT(graph.find_host_sources(Fid{30}) == (llvm::SmallVector<Fid, 4>{Fid{20}}));
    ZASSERT(graph.find_include_chain(Fid{1}, Fid{30}).empty());

    auto readers = graph.find_readers(Fid{30});
    llvm::sort(readers);
    ZASSERT(readers == (llvm::SmallVector<Fid, 4>{Fid{1}, Fid{2}}));
    ZASSERT(graph.find_readers(Fid{10}) == (llvm::SmallVector<Fid, 4>{Fid{1}}));
}

ZEST_CASE(ClosureFollowsForcedIncludes) {
    // Unit 1 includes 10 under one configuration and 11 under another,
    // 10 includes 12, and a forced header 20 includes 30.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}});
    graph.set_includes(Fid{1}, 1, {{Fid{11}}});
    graph.set_includes(Fid{10}, 0, {{Fid{12}}});
    graph.set_includes(Fid{20}, 0, {{Fid{30}}});
    graph.add_forced_include(Fid{1}, Fid{20});

    auto closure = graph.include_closure(Fid{1});
    llvm::SmallVector<Fid> files(closure.begin(), closure.end());
    llvm::sort(files);
    ZEXPECT(files == (llvm::SmallVector<Fid>{Fid{1}, Fid{10}, Fid{11}, Fid{12}, Fid{20}, Fid{30}}));
    ZEXPECT(graph.include_closure(Fid{12}).size() == 1u);
}

ZEST_CASE(CountsDuplicateIncludes) {
    // One edge per directive, under the configuration with the most.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {{Fid{10}}, {Fid{20}}, {Fid{10}}});
    graph.set_includes(Fid{1}, 1, {{Fid{10}}});

    ZEXPECT(graph.count_includes(Fid{1}, Fid{10}) == 2u);
    ZEXPECT(graph.count_includes(Fid{1}, Fid{20}) == 1u);
    ZEXPECT(graph.count_includes(Fid{1}, Fid{30}) == 0u);
}

ZEST_CASE(ImportReachedThroughForced) {
    // Unit 1 forces header 20 in, which includes candidate 30; unit 2
    // includes only header 10.
    clice::DependencyGraph graph;
    graph.set_includes(Fid{20}, 0, {{Fid{30}}});
    graph.set_includes(Fid{2}, 0, {{Fid{10}}});
    graph.add_forced_include(Fid{1}, Fid{20});
    graph.record_scan(Fid{30}, ScanResult{.has_import = true});

    ZEXPECT(graph.reaches_import(Fid{1}));
    ZEXPECT(graph.reaches_import(Fid{30}));
    ZEXPECT(!graph.reaches_import(Fid{2}));
}

};  // ZEST_SUITE(DependencyGraph)

// ============================================================================
// scan_dependency_graph() integration tests
// ============================================================================

ZEST_SUITE(ScanDependencyGraph) {

ZEST_CASE(EmptyCDB) {
    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    scan_all(cdb, graph);

    ZEXPECT(graph.file_count() == 0u);
    ZEXPECT(graph.module_count() == 0u);
    ZEXPECT(graph.edge_count() == 0u);
}

ZEST_CASE(GuardedModuleRuleDefine) {
    /// A module declaration unguarded only by a rule-added define: the
    /// preprocess fallback must scan with the rules-applied command, like
    /// the main scan groups.
    TempDir tmp;
    tmp.touch("src/m.cppm", R"(#ifdef ENABLE_M
export module m;
#endif
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/m.cppm"), {}}
    });
    write_cdb(tmp, cdb, json);
    /// The unit carries its effective command: the edit that unguards the
    /// declaration is applied before the scan sees the file.
    llvm::SmallVector<CommandRef> units;
    for(auto& entry: cdb.entries()) {
        std::vector<std::string> append{"-DENABLE_M"};
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Append, {append.begin(), append.end()}},
        };
        auto applied = cdb.apply_rules(entry.config, {.edits = edits});
        units.push_back({entry.file,
                         applied,
                         cdb.input_kind(applied, cdb.files().resolve(entry.file)),
                         CommandSource::CDBExact});
    }
    scan_dependency_graph(cdb, graph, units);

    ZEXPECT(graph.module_count() == 1u);
    ZEXPECT(graph.lookup_module("m").size() == 1u);
}

ZEST_CASE(GuardedModulePerCandidate) {
    /// Two candidates whose defines select different module names: every
    /// entry is a scan unit preprocessed under its own command, so both
    /// names resolve; a scan handed only the second unit sees only its
    /// name.
    TempDir tmp;
    tmp.touch("src/m.cppm", R"(#ifdef V2
export module m2;
#else
export module m1;
#endif
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/m.cppm"), {}      },
        {tmp.root, tmp.path("src/m.cppm"), {"-DV2"}},
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.lookup_module("m1").size() == 1u);
    ZEXPECT(graph.lookup_module("m2").size() == 1u);

    auto file = file_table.intern(Spelling::absolute(tmp.path("src/m.cppm")));
    auto candidates = cdb.candidate_entries(file);
    ZASSERT(candidates.size() == 2u);
    llvm::SmallVector<CommandRef> v2_units = {
        {file,
         candidates[1].config,
         cdb.input_kind(candidates[1].config, tmp.path("src/m.cppm")),
         CommandSource::CDBExact}
    };
    DependencyGraph graph_v2;
    scan_dependency_graph(cdb, graph_v2, v2_units);
    ZEXPECT(graph_v2.lookup_module("m1").empty());
    ZEXPECT(graph_v2.lookup_module("m2").size() == 1u);

    // A warm run reproduces both: each command preprocesses its own
    // declaration again.
    DependencyGraph graph2;
    scan_all(cdb, graph2);
    ZEXPECT(graph2.lookup_module("m1").size() == 1u);
    ZEXPECT(graph2.lookup_module("m2").size() == 1u);
}

ZEST_CASE(SingleFileNoIncludes) {
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(int main() { return 0; })");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.file_count() == 1u);
    ZEXPECT(graph.edge_count() == 0u);
    ZEXPECT(graph.module_count() == 0u);
}

ZEST_CASE(SingleFileWithInclude) {
    TempDir tmp;
    tmp.touch("include/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
int main() { return x; }
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("include")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.file_count() >= 1u);
    ZEXPECT(graph.edge_count() >= 1u);
}

ZEST_CASE(TransitiveIncludes) {
    TempDir tmp;
    tmp.touch("inc/a.h", R"(#include "b.h")");
    tmp.touch("inc/b.h", R"(#include "c.h")");
    tmp.touch("inc/c.h", R"(int c = 3;)");
    tmp.touch("src/main.cpp", R"(
#include "a.h"
int main() {}
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    // main->a, a->b, b->c across 4 waves.
    ZEXPECT(graph.file_count() >= 3u);
    ZEXPECT(graph.edge_count() >= 3u);
}

ZEST_CASE(MultipleSourceFiles) {
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

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    std::vector<std::string> inc = {"-I", tmp.path("inc")};
    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/a.cpp"), inc},
        {tmp.root, tmp.path("src/b.cpp"), inc},
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.file_count() >= 2u);
    ZEXPECT(graph.edge_count() >= 2u);
}

ZEST_CASE(ConditionalIncludes) {
    TempDir tmp;
    tmp.touch("inc/always.h", R"(// always)");
    tmp.touch("inc/maybe.h", R"(// maybe)");
    tmp.touch("src/main.cpp", R"(
#include "always.h"
#ifdef FOO
#include "maybe.h"
#endif
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    // Both headers discovered (over-approximate).
    ZEXPECT(graph.edge_count() >= 2u);

    // Verify conditional flag.
    bool found_unconditional = false;
    bool found_conditional = false;
    auto includes =
        graph.get_includes(cdb.files().intern(Spelling::absolute(tmp.path("src/main.cpp"))), 0);
    for(auto edge: includes) {
        if(edge.conditional) {
            found_conditional = true;
        } else {
            found_unconditional = true;
        }
    }
    ZEXPECT(found_unconditional);
    ZEXPECT(found_conditional);
}

ZEST_CASE(ModuleExtraction) {
    TempDir tmp;
    tmp.touch("src/mymod.cpp", R"(
export module my.module;
export int foo() { return 42; }
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mymod.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    auto result = graph.lookup_module("my.module");
    ZASSERT(result.size() == 1u);

    auto path = cdb.files().resolve(result[0]);
    ZEXPECT(llvm::sys::fs::equivalent(path, tmp.path("src/mymod.cpp")));
}

ZEST_CASE(ModulePartition) {
    TempDir tmp;
    tmp.touch("src/mod.cpp", R"(
export module my.mod:part;
void impl() {}
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mod.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZASSERT(graph.lookup_module("my.mod:part").size() == 1u);
}

ZEST_CASE(DiamondIncludes) {
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

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    // main->a, main->b, a->common, b->common.
    ZEXPECT(graph.edge_count() >= 4u);
    ZEXPECT(graph.file_count() >= 3u);
}

ZEST_CASE(AngledVsQuoted) {
    TempDir tmp;
    tmp.touch("quoted/header.h", R"(int q = 1;)");
    tmp.touch("angled/header.h", R"(int a = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
#include <header.h>
int main() {}
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root,
         tmp.path("src/main.cpp"),
         {"-iquote", tmp.path("quoted"), "-I", tmp.path("angled")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.edge_count() >= 2u);
}

ZEST_CASE(MissingInclude) {
    TempDir tmp;
    tmp.touch("src/main.cpp", R"(
#include "nonexistent.h"
int main() {}
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.file_count() == 1u);
    ZEXPECT(graph.edge_count() == 0u);
}

ZEST_CASE(ForcedIncludeScanned) {
    // A forced header is a scanned node: its includes are edges, its
    // import syntax reaches the candidate set, and its unit reaches it
    // through the forced relation only.
    TempDir tmp;
    tmp.touch("build/force.h", R"(#include "dep.h"
import m;
)");
    tmp.touch("build/dep.h", "int dep;\n");
    tmp.touch("src/main.cpp", "int main() {}\n");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.path("build"), tmp.path("src/main.cpp"), {"-include", "force.h"}}
    }));
    scan_all(cdb, graph);
    graph.build_reverse_map();

    auto main = file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto force = file_table.intern(Spelling::absolute(tmp.path("build/force.h")));
    auto dep = file_table.intern(Spelling::absolute(tmp.path("build/dep.h")));
    ZEXPECT(graph.get_forcing_units(force) == llvm::ArrayRef<Fid>{main});
    ZEXPECT(graph.get_all_includes(main).empty());
    ZEXPECT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{dep});
    ZEXPECT(graph.reaches_import(main));
    ZEXPECT(graph.find_readers(dep) == (llvm::SmallVector<Fid, 4>{main}));
}

ZEST_CASE(ForcedIncludeLookupOrder) {
    // As clang does: the compile's working directory first, then the
    // search path as for a quoted include.
    TempDir tmp;
    tmp.touch("build/first.h", "\n");
    tmp.touch("inc/first.h", "\n");
    tmp.touch("inc/second.h", "\n");
    tmp.touch("src/main.cpp", "int main() {}\n");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.path("build"),
                   tmp.path("src/main.cpp"),
                   {"-I", tmp.path("inc"), "-include", "first.h", "-include", "second.h"}}
    }));
    scan_all(cdb, graph);

    auto main = file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto forced = [&](llvm::StringRef path) {
        return graph.get_forcing_units(file_table.intern(Spelling::absolute(tmp.path(path))));
    };
    ZEXPECT(forced("build/first.h") == llvm::ArrayRef<Fid>{main});
    ZEXPECT(forced("inc/first.h").empty());
    ZEXPECT(forced("inc/second.h") == llvm::ArrayRef<Fid>{main});
}

ZEST_CASE(RescanScansNewHeader) {
    // A save that includes a file the scan never reached scans it under
    // the includer's context: its own includes and import syntax join.
    TempDir tmp;
    tmp.touch("inc/a.h", "\n");
    tmp.touch("inc/b.h", "#include <c.h>\nimport m;\n");
    tmp.touch("inc/c.h", "\n");
    tmp.touch("src/main.cpp", "#include <a.h>\n");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    }));
    scan_all(cdb, graph);
    graph.build_reverse_map();
    auto main = file_table.intern(Spelling::absolute(tmp.path("src/main.cpp")));
    auto b = file_table.intern(Spelling::absolute(tmp.path("inc/b.h")));
    auto c = file_table.intern(Spelling::absolute(tmp.path("inc/c.h")));
    ZEXPECT(!graph.reaches_import(main));

    tmp.touch("src/main.cpp", "#include <a.h>\n#include <b.h>\n");
    rescan_dependency_graph(cdb, graph, main);
    ZEXPECT(graph.get_all_includes(b) == llvm::SmallVector<Fid>{c});
    ZEXPECT(graph.reaches_import(main));
}

ZEST_CASE(RescanResumesIncludeNext) {
    // A rescan resumes #include_next after the directory the scan found
    // the file in, as the full scan and clang do.
    TempDir tmp;
    tmp.touch("z/other.h", "\n");
    tmp.touch("a/x.h", "#include_next <x.h>\n");
    tmp.touch("b/x.h", "\n");
    tmp.touch("src/main.cpp", "#include <x.h>\n");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root,
                   tmp.path("src/main.cpp"),
                   {"-I", tmp.path("z"), "-I", tmp.path("a"), "-I", tmp.path("b")}}
    }));
    scan_all(cdb, graph);
    graph.build_reverse_map();
    auto wrapper = file_table.intern(Spelling::absolute(tmp.path("a/x.h")));
    auto next = file_table.intern(Spelling::absolute(tmp.path("b/x.h")));
    ZASSERT(graph.get_all_includes(wrapper) == llvm::SmallVector<Fid>{next});

    tmp.touch("a/x.h", "#include_next <x.h>\n#define CHANGED\n");
    rescan_dependency_graph(cdb, graph, wrapper);
    ZEXPECT(graph.get_all_includes(wrapper) == llvm::SmallVector<Fid>{next});
}

ZEST_CASE(RescanForcedUnderUnit) {
    // A rescanned forced header resolves its includes under the command
    // forcing it in, as the full scan did — not under a nearer unit's.
    TempDir tmp;
    tmp.touch("a/cfg.h", "\n");
    tmp.touch("b/cfg.h", "\n");
    tmp.touch("build/force.h", "#include <cfg.h>\n");
    tmp.touch("src/main.cpp", "\n");
    tmp.touch("build/near.cpp", "\n");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root,
                   tmp.path("src/main.cpp"),
                   {"-I", tmp.path("a"), "-include", tmp.path("build/force.h")}},
                  {tmp.root, tmp.path("build/near.cpp"), {"-I", tmp.path("b")} },
    }));
    scan_all(cdb, graph);
    graph.build_reverse_map();

    auto force = file_table.intern(Spelling::absolute(tmp.path("build/force.h")));
    auto cfg = file_table.intern(Spelling::absolute(tmp.path("a/cfg.h")));
    ZASSERT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{cfg});
    tmp.touch("build/force.h", "#include <cfg.h>\n#define CHANGED\n");
    rescan_dependency_graph(cdb, graph, force);
    ZEXPECT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{cfg});
}

ZEST_CASE(MultipleModules) {
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

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mod_a.cpp"), {}},
        {tmp.root, tmp.path("src/mod_b.cpp"), {}},
        {tmp.root, tmp.path("src/impl.cpp"),  {}},
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZEXPECT(graph.module_count() == 2u);
    ZASSERT(!graph.lookup_module("mod.a").empty());
    ZASSERT(!graph.lookup_module("mod.b").empty());
}

ZEST_CASE(DeepIncludeChain) {
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

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    // main->h0->h1->h2->h3->h4 across 5 waves.
    ZEXPECT(graph.edge_count() >= 5u);
    ZEXPECT(graph.file_count() >= 5u);
}

ZEST_CASE(ModuleWithIncludes) {
    TempDir tmp;
    tmp.touch("inc/util.h", R"(int util = 1;)");
    tmp.touch("src/mymod.cpp", R"(
module;
#include "util.h"
export module my.lib;
export int value() { return util; }
)");

    FileTable file_table;
    CompilationDatabase cdb{file_table};
    DependencyGraph graph;

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/mymod.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);
    scan_all(cdb, graph);

    ZASSERT(!graph.lookup_module("my.lib").empty());
    ZEXPECT(graph.edge_count() >= 1u);
}

ZEST_CASE(ScanCacheWarmRun) {
    TempDir tmp;
    tmp.touch("inc/util.h", R"(int util = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "util.h"
int main() {}
)");
    // Out of the mtime guard window, or the cold scan's pairs stay
    // unreliable and cannot vouch for the warm run.
    ZASSERT(set_file_mtime(tmp.path("inc/util.h"),
                           file_mtime_ns(tmp.path("inc/util.h")) - 10'000'000'000));
    ZASSERT(set_file_mtime(tmp.path("src/main.cpp"),
                           file_mtime_ns(tmp.path("src/main.cpp")) - 10'000'000'000));

    FileTable file_table;
    CompilationDatabase cdb{file_table};

    auto json = build_cdb_json({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    write_cdb(tmp, cdb, json);

    DependencyGraph graph;
    auto cold = scan_all(cdb, graph);
    ZEXPECT(graph.edge_count() >= 1u);

    // A rescan against the same shared table skips the read and the lex
    // for every unchanged file (stat-validated through the shared pairs).
    DependencyGraph graph2;
    auto warm = scan_all(cdb, graph2);
    ZEXPECT(warm.scan_cache_hits > std::size_t(0));
    ZEXPECT(graph2.edge_count() == graph.edge_count());
    ZEXPECT(graph2.file_count() == graph.file_count());
}

// TODO: add tests for:
// - Circular includes (A→B→A) to verify BFS terminates correctly
// - get_all_includes across configs: a header in one config but not
//   another appears once in the deduped union (the union carries plain
//   fids — conditionality is per-config, asserted via get_includes)
// - set_includes overwrite: calling twice with same (path_id, config_id)

};  // ZEST_SUITE(ScanDependencyGraph)

}  // namespace
}  // namespace clice::testing
