module clice;

import :command.database;
import :command.edit;
import :command.table;
import :command.toolchain;
import :syntax.dependency_graph;
import :syntax.scan;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_table;

namespace clice::testing {
namespace {

ZEST_SUITE(DependencyGraph) {

// ============================================================================
// Module mapping tests
// ============================================================================

ZEST_CASE(AddAndLookupModule) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.lookup_module("foo.bar").empty());
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
    ZEXPECT(ranges::contains(graph.lookup_module("a"), Fid{1}));
    ZEXPECT(ranges::contains(graph.lookup_module("b"), Fid{1}));
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
    ZEXPECT(graph.modules().size() == 0u);

    graph.add_module("a", Fid{1});
    ZEXPECT(graph.modules().size() == 1u);

    graph.add_module("b", Fid{2});
    ZEXPECT(graph.modules().size() == 2u);

    // Second candidate for "a" doesn't increase module name count.
    graph.add_module("a", Fid{3});
    ZEXPECT(graph.modules().size() == 2u);
}

// ============================================================================
// Include edge tests
// ============================================================================

ZEST_CASE(SetAndGetIncludes) {
    clice::DependencyGraph graph;
    ZEXPECT(graph.get_includes(Fid{1}, 0).empty());
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
    ZEXPECT(ranges::contains(all, Fid{10}));
    ZEXPECT(ranges::contains(all, Fid{20}));
    ZEXPECT(ranges::contains(all, Fid{30}));
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

ZEST_CASE(EmptyIncludes) {
    clice::DependencyGraph graph;
    graph.set_includes(Fid{1}, 0, {});

    ZEXPECT(graph.get_includes(Fid{1}, 0).empty());
    ZEXPECT(graph.knows(Fid{1}));
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
    ranges::sort(readers);
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
    ranges::sort(files);
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

/// A database of files in a temporary directory and the graph its scan
/// builds.
struct ScanFixture {
    TempDir tmp;
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase cdb{file_table, commands};
    Toolchain toolchain{commands, file_table};
    DependencyGraph graph;

    /// List `entries` in the database and scan each under its own command.
    void scan(llvm::ArrayRef<CDBEntry> entries) {
        write_cdb(tmp, cdb, build_cdb_json(entries));
        scan_all(cdb, toolchain, graph);
    }

    /// The file at `relative` in the temporary directory.
    Fid id(llvm::StringRef relative) {
        return file_table.intern(Spelling::absolute(tmp.path(relative)));
    }

    /// What the file at `relative` includes, under every configuration.
    llvm::SmallVector<Fid> includes(llvm::StringRef relative) {
        return graph.get_all_includes(id(relative));
    }
};

ZEST_SUITE(ScanDependencyGraph, ScanFixture) {

ZEST_CASE(EmptyCDB) {
    scan_all(cdb, toolchain, graph);
    graph.build_reverse_map();

    ZEXPECT(graph.all_files().empty());
    ZEXPECT(graph.modules().empty());
}

ZEST_CASE(GuardedModuleRuleDefine) {
    /// A module declaration unguarded only by a rule-added define: the
    /// preprocess fallback must scan with the rules-applied command, like
    /// the main scan groups.
    tmp.touch("src/m.cppm", R"(#ifdef ENABLE_M
export module m;
#endif
)");

    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("src/m.cppm"), {}}
    }));
    /// The unit carries its effective command: the edit that unguards the
    /// declaration is applied before the scan sees the file.
    llvm::SmallVector<CommandRef> units;
    for(auto& entry: cdb.entries()) {
        std::vector<std::string> append{"-DENABLE_M"};
        std::vector<CommandEdit> edits = {
            {CommandEdit::Kind::Append, {append.begin(), append.end()}},
        };
        auto unit = database_command(cdb, entry);
        unit.command = apply_edits(commands, file_table, entry.command, {.edits = edits});
        units.push_back(unit);
    }
    scan_dependency_graph(cdb, toolchain, graph, units);

    ZEXPECT(graph.modules().size() == 1u);
    ZEXPECT(graph.lookup_module("m").size() == 1u);
}

ZEST_CASE(GuardedModulePerCandidate) {
    /// Two candidates whose defines select different module names: every
    /// entry is a scan unit preprocessed under its own command, so both
    /// names resolve; a scan handed only the second unit sees only its
    /// name.
    tmp.touch("src/m.cppm", R"(#ifdef V2
export module m2;
#else
export module m1;
#endif
)");

    scan({
        {tmp.root, tmp.path("src/m.cppm"), {}      },
        {tmp.root, tmp.path("src/m.cppm"), {"-DV2"}},
    });

    ZEXPECT(graph.lookup_module("m1").size() == 1u);
    ZEXPECT(graph.lookup_module("m2").size() == 1u);

    auto file = id("src/m.cppm");
    auto candidates = cdb.candidate_entries(file);
    ZASSERT(candidates.size() == 2u);
    llvm::SmallVector<CommandRef> v2_units = {database_command(cdb, candidates[1])};
    DependencyGraph graph_v2;
    scan_dependency_graph(cdb, toolchain, graph_v2, v2_units);
    ZEXPECT(graph_v2.lookup_module("m1").empty());
    ZEXPECT(graph_v2.lookup_module("m2").size() == 1u);

    // A warm run reproduces both: each command preprocesses its own
    // declaration again.
    graph.reset();
    scan_all(cdb, toolchain, graph);
    ZEXPECT(graph.lookup_module("m1").size() == 1u);
    ZEXPECT(graph.lookup_module("m2").size() == 1u);
}

ZEST_CASE(OutputsShareScanGroup) {
    /// Commands that differ only in their output resolve includes alike:
    /// they share one scan group, and a file listed under both is one
    /// context.
    tmp.touch("inc/h.h", R"(int h;)");
    tmp.touch("src/a.cpp", R"(#include "h.h")");
    tmp.touch("src/b.cpp", R"(#include "h.h")");

    scan({
        {tmp.root, tmp.path("src/a.cpp"), {"-I", tmp.path("inc"), "-o", "a.o"} },
        {tmp.root, tmp.path("src/b.cpp"), {"-I", tmp.path("inc"), "-o", "b.o"} },
        {tmp.root, tmp.path("src/a.cpp"), {"-I", tmp.path("inc"), "-o", "a2.o"}},
    });

    ZEXPECT(graph.groups().size() == 1u);
    auto a = id("src/a.cpp");
    ZEXPECT(graph.contexts(a).size() == 1u);
}

ZEST_CASE(SingleFileNoIncludes) {
    tmp.touch("src/main.cpp", R"(int main() { return 0; })");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });

    graph.build_reverse_map();
    ZEXPECT(graph.all_files() == llvm::SmallVector<Fid>{id("src/main.cpp")});
    ZEXPECT(graph.modules().empty());
}

ZEST_CASE(SingleFileWithInclude) {
    tmp.touch("include/header.h", R"(int x = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
int main() { return x; }
)");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("include")}}
    });

    ZEXPECT(includes("src/main.cpp") == llvm::SmallVector<Fid>{id("include/header.h")});
}

ZEST_CASE(TransitiveIncludes) {
    tmp.touch("inc/a.h", R"(#include "b.h")");
    tmp.touch("inc/b.h", R"(#include "c.h")");
    tmp.touch("inc/c.h", R"(int c = 3;)");
    tmp.touch("src/main.cpp", R"(
#include "a.h"
int main() {}
)");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });

    ZEXPECT(includes("src/main.cpp") == llvm::SmallVector<Fid>{id("inc/a.h")});
    ZEXPECT(includes("inc/a.h") == llvm::SmallVector<Fid>{id("inc/b.h")});
    ZEXPECT(includes("inc/b.h") == llvm::SmallVector<Fid>{id("inc/c.h")});
    ZEXPECT(includes("inc/c.h").empty());
}

ZEST_CASE(MultipleSourceFiles) {
    tmp.touch("inc/shared.h", R"(int shared = 1;)");
    tmp.touch("src/a.cpp", R"(
#include "shared.h"
void a() {}
)");
    tmp.touch("src/b.cpp", R"(
#include "shared.h"
void b() {}
)");

    std::vector<std::string> inc = {"-I", tmp.path("inc")};
    scan({
        {tmp.root, tmp.path("src/a.cpp"), inc},
        {tmp.root, tmp.path("src/b.cpp"), inc},
    });

    ZEXPECT(includes("src/a.cpp") == llvm::SmallVector<Fid>{id("inc/shared.h")});
    ZEXPECT(includes("src/b.cpp") == llvm::SmallVector<Fid>{id("inc/shared.h")});
}

ZEST_CASE(ConditionalIncludes) {
    tmp.touch("inc/always.h", R"(// always)");
    tmp.touch("inc/maybe.h", R"(// maybe)");
    tmp.touch("src/main.cpp", R"(
#include "always.h"
#ifdef FOO
#include "maybe.h"
#endif
)");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });

    // Both headers discovered (over-approximate), the guarded one marked.
    auto edges = graph.get_includes(id("src/main.cpp"), 0);
    ZASSERT(edges.size() == 2u);
    ZEXPECT(edges[0].fid == id("inc/always.h"));
    ZEXPECT(!edges[0].conditional);
    ZEXPECT(edges[1].fid == id("inc/maybe.h"));
    ZEXPECT(edges[1].conditional);
}

ZEST_CASE(ModuleExtraction) {
    tmp.touch("src/mymod.cpp", R"(
export module my.module;
export int foo() { return 42; }
)");

    scan({
        {tmp.root, tmp.path("src/mymod.cpp"), {}}
    });

    auto result = graph.lookup_module("my.module");
    ZASSERT(result.size() == 1u);

    auto path = file_table.resolve(result[0]);
    ZEXPECT(fs::equivalent(path, tmp.path("src/mymod.cpp")).value_or(false));
}

ZEST_CASE(ModulePartition) {
    tmp.touch("src/mod.cpp", R"(
export module my.mod:part;
void impl() {}
)");

    scan({
        {tmp.root, tmp.path("src/mod.cpp"), {}}
    });

    ZASSERT(graph.lookup_module("my.mod:part").size() == 1u);
}

ZEST_CASE(InternalPartitionProvidesModule) {
    tmp.touch("src/lib.cppm", "export module lib;\n");
    tmp.touch("src/detail.cppm", "module lib:detail;\n");
    tmp.touch("src/guarded.cppm", R"(#ifdef GUARDED
module lib:guarded;
#endif
)");
    tmp.touch("src/impl.cpp", "module lib;\nimport :detail;\n");

    scan({
        {tmp.root, tmp.path("src/lib.cppm"),     {}           },
        {tmp.root, tmp.path("src/detail.cppm"),  {}           },
        {tmp.root, tmp.path("src/guarded.cppm"), {"-DGUARDED"}},
        {tmp.root, tmp.path("src/impl.cpp"),     {}           },
    });

    auto provider = [&](llvm::StringRef name) -> std::string {
        auto ids = graph.lookup_module(name);
        return ids.size() == 1 ? file_table.resolve(ids[0]).str() : "";
    };
    ZEXPECT(fs::equivalent(provider("lib"), tmp.path("src/lib.cppm")).value_or(false));
    ZEXPECT(fs::equivalent(provider("lib:detail"), tmp.path("src/detail.cppm")).value_or(false));
    ZEXPECT(fs::equivalent(provider("lib:guarded"), tmp.path("src/guarded.cppm")).value_or(false));
    ZEXPECT(graph.modules().size() == 3u);
    ZEXPECT(!graph.internal_partition(graph.lookup_module("lib").front()));
    ZEXPECT(graph.internal_partition(graph.lookup_module("lib:detail").front()));
    ZEXPECT(graph.internal_partition(graph.lookup_module("lib:guarded").front()));

    auto impl = id("src/impl.cpp");
    ZEXPECT(graph.module_of(impl).empty());
}

ZEST_CASE(DiamondIncludes) {
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

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });

    ZEXPECT(includes("src/main.cpp") == (llvm::SmallVector<Fid>{id("inc/a.h"), id("inc/b.h")}));
    ZEXPECT(includes("inc/a.h") == llvm::SmallVector<Fid>{id("inc/common.h")});
    ZEXPECT(includes("inc/b.h") == llvm::SmallVector<Fid>{id("inc/common.h")});
}

ZEST_CASE(AngledVsQuoted) {
    tmp.touch("quoted/header.h", R"(int q = 1;)");
    tmp.touch("angled/header.h", R"(int a = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "header.h"
#include <header.h>
int main() {}
)");

    scan({
        {tmp.root,
         tmp.path("src/main.cpp"),
         {"-iquote", tmp.path("quoted"), "-I", tmp.path("angled")}}
    });

    ZEXPECT(includes("src/main.cpp") ==
            (llvm::SmallVector<Fid>{id("quoted/header.h"), id("angled/header.h")}));
}

ZEST_CASE(MissingInclude) {
    tmp.touch("src/main.cpp", R"(
#include "nonexistent.h"
int main() {}
)");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });

    graph.build_reverse_map();
    ZEXPECT(graph.all_files() == llvm::SmallVector<Fid>{id("src/main.cpp")});
}

ZEST_CASE(ForcedIncludeScanned) {
    // A forced header is a scanned node: its includes are edges, its
    // import syntax reaches the candidate set, and its unit reaches it
    // through the forced relation only.
    tmp.touch("build/force.h", R"(#include "dep.h"
import m;
)");
    tmp.touch("build/dep.h", "int dep;\n");
    tmp.touch("src/main.cpp", "int main() {}\n");

    scan({
        {tmp.path("build"), tmp.path("src/main.cpp"), {"-include", "force.h"}}
    });
    graph.build_reverse_map();

    auto main = id("src/main.cpp");
    auto force = id("build/force.h");
    auto dep = id("build/dep.h");
    ZEXPECT(graph.get_forcing_units(force) == llvm::ArrayRef<Fid>{main});
    ZEXPECT(graph.get_all_includes(main).empty());
    ZEXPECT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{dep});
    ZEXPECT(graph.reaches_import(main));
    ZEXPECT(graph.find_readers(dep) == (llvm::SmallVector<Fid, 4>{main}));
}

ZEST_CASE(ForcedIncludeLookupOrder) {
    // As clang does: the compile's working directory first, then the
    // search path as for a quoted include.
    tmp.touch("build/first.h", "\n");
    tmp.touch("inc/first.h", "\n");
    tmp.touch("inc/second.h", "\n");
    tmp.touch("src/main.cpp", "int main() {}\n");

    scan({
        {tmp.path("build"),
         tmp.path("src/main.cpp"),
         {"-I", tmp.path("inc"), "-include", "first.h", "-include", "second.h"}}
    });

    auto main = id("src/main.cpp");
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
    tmp.touch("inc/a.h", "\n");
    tmp.touch("inc/b.h", "#include <c.h>\nimport m;\n");
    tmp.touch("inc/c.h", "\n");
    tmp.touch("src/main.cpp", "#include <a.h>\n");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    graph.build_reverse_map();
    auto main = id("src/main.cpp");
    auto b = id("inc/b.h");
    auto c = id("inc/c.h");
    ZEXPECT(!graph.reaches_import(main));

    tmp.touch("src/main.cpp", "#include <a.h>\n#include <b.h>\n");
    rescan_dependency_graph(cdb, toolchain, graph, main);
    ZEXPECT(graph.get_all_includes(b) == llvm::SmallVector<Fid>{c});
    ZEXPECT(graph.reaches_import(main));
}

ZEST_CASE(RescanLeavesUnreachedOut) {
    // A save of a file the scan never reached records nothing: no unit
    // compiles it, so its import syntax opens no gate.
    tmp.touch("src/main.cpp", "int main() { return 0; }\n");
    tmp.touch("src/scratch.h", "import m;\n");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {}}
    });
    graph.build_reverse_map();
    auto scratch = id("src/scratch.h");

    rescan_dependency_graph(cdb, toolchain, graph, scratch);
    ZEXPECT(!graph.has_import_candidates());
    ZEXPECT(!graph.scanned(scratch, scan_quick("import m;\n").directives_hash));
}

ZEST_CASE(RescanTracksPartitionKind) {
    tmp.touch("src/part.cppm", "export module lib:part;\n");

    scan({
        {tmp.root, tmp.path("src/part.cppm"), {}}
    });
    auto part = id("src/part.cppm");
    ZEXPECT(!graph.internal_partition(part));

    tmp.touch("src/part.cppm", "module lib:part;\n");
    rescan_dependency_graph(cdb, toolchain, graph, part);
    ZEXPECT(graph.module_of(part) == "lib:part");
    ZEXPECT(graph.internal_partition(part));

    tmp.touch("src/part.cppm", "export module lib:part;\n");
    rescan_dependency_graph(cdb, toolchain, graph, part);
    ZEXPECT(!graph.internal_partition(part));

    tmp.touch("src/part.cppm", "module lib:part;\n");
    rescan_dependency_graph(cdb, toolchain, graph, part);
    ZEXPECT(graph.internal_partition(part));

    tmp.touch("src/part.cppm", "module lib;\n");
    rescan_dependency_graph(cdb, toolchain, graph, part);
    ZEXPECT(graph.module_of(part).empty());
    ZEXPECT(!graph.internal_partition(part));
}

ZEST_CASE(RescanResumesIncludeNext) {
    // A rescan resumes #include_next after the directory the scan found
    // the file in, as the full scan and clang do.
    tmp.touch("z/other.h", "\n");
    tmp.touch("a/x.h", "#include_next <x.h>\n");
    tmp.touch("b/x.h", "\n");
    tmp.touch("src/main.cpp", "#include <x.h>\n");

    scan({
        {tmp.root,
         tmp.path("src/main.cpp"),
         {"-I", tmp.path("z"), "-I", tmp.path("a"), "-I", tmp.path("b")}}
    });
    graph.build_reverse_map();
    auto wrapper = id("a/x.h");
    auto next = id("b/x.h");
    ZASSERT(graph.get_all_includes(wrapper) == llvm::SmallVector<Fid>{next});

    tmp.touch("a/x.h", "#include_next <x.h>\n#define CHANGED\n");
    rescan_dependency_graph(cdb, toolchain, graph, wrapper);
    ZEXPECT(graph.get_all_includes(wrapper) == llvm::SmallVector<Fid>{next});
}

ZEST_CASE(RescanForcedUnderUnit) {
    // A rescanned forced header resolves its includes under the command
    // forcing it in, as the full scan did — not under a nearer unit's.
    tmp.touch("a/cfg.h", "\n");
    tmp.touch("b/cfg.h", "\n");
    tmp.touch("build/force.h", "#include <cfg.h>\n");
    tmp.touch("src/main.cpp", "\n");
    tmp.touch("build/near.cpp", "\n");

    scan({
        {tmp.root,
         tmp.path("src/main.cpp"),
         {"-I", tmp.path("a"), "-include", tmp.path("build/force.h")}},
        {tmp.root, tmp.path("build/near.cpp"), {"-I", tmp.path("b")} },
    });
    graph.build_reverse_map();

    auto force = id("build/force.h");
    auto cfg = id("a/cfg.h");
    ZASSERT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{cfg});
    tmp.touch("build/force.h", "#include <cfg.h>\n#define CHANGED\n");
    rescan_dependency_graph(cdb, toolchain, graph, force);
    ZEXPECT(graph.get_all_includes(force) == llvm::SmallVector<Fid>{cfg});
}

ZEST_CASE(MultipleModules) {
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

    scan({
        {tmp.root, tmp.path("src/mod_a.cpp"), {}},
        {tmp.root, tmp.path("src/mod_b.cpp"), {}},
        {tmp.root, tmp.path("src/impl.cpp"),  {}},
    });

    ZEXPECT(graph.modules().size() == 2u);
    ZASSERT(!graph.lookup_module("mod.a").empty());
    ZASSERT(!graph.lookup_module("mod.b").empty());
}

ZEST_CASE(DeepIncludeChain) {
    tmp.touch("inc/h4.h", R"(int h4 = 4;)");
    tmp.touch("inc/h3.h", R"(#include "h4.h")");
    tmp.touch("inc/h2.h", R"(#include "h3.h")");
    tmp.touch("inc/h1.h", R"(#include "h2.h")");
    tmp.touch("inc/h0.h", R"(#include "h1.h")");
    tmp.touch("src/main.cpp", R"(
#include "h0.h"
int main() {}
)");

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });

    ZEXPECT(includes("src/main.cpp") == llvm::SmallVector<Fid>{id("inc/h0.h")});
    for(int depth = 0; depth < 4; depth += 1) {
        ZEXPECT(includes(std::format("inc/h{}.h", depth)) ==
                llvm::SmallVector<Fid>{id(std::format("inc/h{}.h", depth + 1))});
    }
    ZEXPECT(includes("inc/h4.h").empty());
}

ZEST_CASE(ModuleWithIncludes) {
    tmp.touch("inc/util.h", R"(int util = 1;)");
    tmp.touch("src/mymod.cpp", R"(
module;
#include "util.h"
export module my.lib;
export int value() { return util; }
)");

    scan({
        {tmp.root, tmp.path("src/mymod.cpp"), {"-I", tmp.path("inc")}}
    });

    ZEXPECT(graph.lookup_module("my.lib") == llvm::ArrayRef<Fid>{id("src/mymod.cpp")});
    ZEXPECT(includes("src/mymod.cpp") == llvm::SmallVector<Fid>{id("inc/util.h")});
}

ZEST_CASE(ScanCacheWarmRun) {
    tmp.touch("inc/util.h", R"(int util = 1;)");
    tmp.touch("src/main.cpp", R"(
#include "util.h"
int main() {}
)");
    // Out of the mtime guard window, or the cold scan's pairs stay
    // unreliable and cannot confirm the warm run.
    age_file(tmp.path("inc/util.h"));
    age_file(tmp.path("src/main.cpp"));

    scan({
        {tmp.root, tmp.path("src/main.cpp"), {"-I", tmp.path("inc")}}
    });
    ZASSERT(includes("src/main.cpp") == llvm::SmallVector<Fid>{id("inc/util.h")});

    // A rebuild keeps the scans: the rescan skips the read and the lex for
    // every unchanged file (stat-validated through the shared pairs).
    graph.reset();
    auto warm = scan_all(cdb, toolchain, graph);
    ZEXPECT(warm.scan_cache_hits == std::size_t(2));
    ZEXPECT(includes("src/main.cpp") == llvm::SmallVector<Fid>{id("inc/util.h")});
}

// TODO: add tests for:
// - Circular includes (A→B→A) to verify BFS terminates correctly
// - set_includes overwrite: calling twice with same (path_id, config_id)

};  // ZEST_SUITE(ScanDependencyGraph)

}  // namespace
}  // namespace clice::testing
