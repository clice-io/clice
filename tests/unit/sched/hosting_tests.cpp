#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "sched/hosting.h"
#include "sched/workspace.h"

namespace clice::testing {

namespace {

TEST_SUITE(Hosting) {

TEST_CASE(SourcePriorityBeatsProximity) {
    /// Two units include the header; the one compiled from the database
    /// the header's rule names ranks first even though the other sits next
    /// to the header, and it is the default host.
    TempDir tmp;
    tmp.touch("lib/x.h", "");
    tmp.touch("lib/near.cpp", R"(#include "x.h")");
    tmp.touch("src/far.cpp", R"(#include "../lib/x.h")");
    tmp.touch("cmake/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("lib/near.cpp"), {}}
    }));
    tmp.touch("lib/cmake/compile_commands.json",
              build_cdb_json({
                  {tmp.root, tmp.path("src/far.cpp"), {}}
    }));

    Workspace workspace;
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"lib/**"}, .compile_commands = {"lib/cmake"}});
    workspace.config.rules.push_back(ConfigRule{.compile_commands = {"cmake"}});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    for(auto source: workspace.build.declared_sources()) {
        workspace.cdb.load(source);
    }

    auto header = workspace.file_table.intern(tmp.path("lib/x.h"));
    auto near = workspace.file_table.intern(tmp.path("lib/near.cpp"));
    auto far = workspace.file_table.intern(tmp.path("src/far.cpp"));
    workspace.dep_graph.set_includes(near, 0, {{header}});
    workspace.dep_graph.set_includes(far, 0, {{header}});
    workspace.dep_graph.build_reverse_map();

    auto ranked = ranked_hosts(workspace, header);
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], far);
    EXPECT_EQ(ranked[1], near);
    auto host = default_host(workspace, header);
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(host->file, far);
    EXPECT_EQ(host->chain.back(), header);
};

TEST_CASE(ProximityWithinSource) {
    /// Same database: the unit sharing the header's stem wins, then the one
    /// in its directory; a unit the build does not compile is no host.
    TempDir tmp;
    tmp.touch("src/x.h", "");
    Workspace workspace;
    workspace.config.rules.push_back(ConfigRule{
        .patterns = {"src/**", "other/**"},
        .default_command = std::string("clang++")
    });
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");

    auto header = workspace.file_table.intern(tmp.path("src/x.h"));
    auto same_stem = workspace.file_table.intern(tmp.path("other/x.cpp"));
    auto same_dir = workspace.file_table.intern(tmp.path("src/y.cpp"));
    auto elsewhere = workspace.file_table.intern(tmp.path("other/z.cpp"));
    auto not_compiled = workspace.file_table.intern(tmp.path("skip/w.cpp"));
    for(auto host: {same_stem, same_dir, elsewhere, not_compiled}) {
        workspace.dep_graph.set_includes(host, 0, {{header}});
    }
    workspace.dep_graph.build_reverse_map();

    auto ranked = ranked_hosts(workspace, header);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0], same_stem);
    EXPECT_EQ(ranked[1], same_dir);
    EXPECT_EQ(ranked[2], elsewhere);

    /// Equal scores fall back to path order, so the ranking is stable.
    auto first = workspace.file_table.intern(tmp.path("other/aaa.cpp"));
    auto second = workspace.file_table.intern(tmp.path("other/bbb.cpp"));
    workspace.dep_graph.set_includes(second, 0, {{header}});
    workspace.dep_graph.set_includes(first, 0, {{header}});
    workspace.dep_graph.build_reverse_map();
    ranked = ranked_hosts(workspace, header);
    ASSERT_EQ(ranked.size(), 5u);
    EXPECT_EQ(ranked[2], first);
    EXPECT_EQ(ranked[3], second);
    EXPECT_EQ(ranked[4], elsewhere);
};

};  // TEST_SUITE(Hosting)

}  // namespace

}  // namespace clice::testing
