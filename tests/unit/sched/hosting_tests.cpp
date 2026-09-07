#include <format>

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

TEST_CASE(HostsMatchLanguage) {
    /// A C unit never hosts a C++ header; an ambiguous `.h` takes any host.
    TempDir tmp;
    tmp.touch("shared/types.hpp", "");
    tmp.touch("shared/plain.h", "");
    Workspace workspace;
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"c/**"}, .default_command = std::string("clang")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");

    auto hpp = workspace.file_table.intern(tmp.path("shared/types.hpp"));
    auto plain = workspace.file_table.intern(tmp.path("shared/plain.h"));
    auto impl = workspace.file_table.intern(tmp.path("c/impl.c"));
    workspace.dep_graph.set_includes(impl, 0, {{hpp}, {plain}});
    workspace.dep_graph.build_reverse_map();

    EXPECT_TRUE(ranked_hosts(workspace, hpp).empty());
    EXPECT_EQ(ranked_hosts(workspace, plain), llvm::SmallVector<Fid>{impl});

    /// A CUDA unit is C++ with device code: it hosts a C++ header.
    tmp.touch("gpu/kernel.cu", "");
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"gpu/**"}, .default_command = std::string("clang++ -x cuda")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    auto kernel = workspace.file_table.intern(tmp.path("gpu/kernel.cu"));
    workspace.dep_graph.set_includes(kernel, 0, {{hpp}});
    workspace.dep_graph.build_reverse_map();
    EXPECT_EQ(ranked_hosts(workspace, hpp), llvm::SmallVector<Fid>{kernel});

    /// Only headers get that latitude: a C++ source borrowing the CUDA
    /// command would compile as CUDA.
    auto gpu_header = workspace.file_table.intern(tmp.path("gpu/new.hpp"));
    auto gpu_source = workspace.file_table.intern(tmp.path("gpu/new.cpp"));
    EXPECT_EQ(command_lender(workspace, gpu_header)->unit, kernel);
    EXPECT_FALSE(command_lender(workspace, gpu_source).has_value());
};

TEST_CASE(LenderSibling) {
    /// A file without a command borrows from a unit in its directory, the
    /// one sharing its stem before the first by name; a `.c` only from a C
    /// unit, and nothing when the build has none.
    TempDir tmp;
    tmp.touch("src/aaa.cpp", "");
    tmp.touch("src/x.cpp", "");
    tmp.touch("src/x.h", "");
    tmp.touch("src/new.cpp", "");
    tmp.touch("src/plain.c", "");
    Workspace workspace;
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"src/*.cpp"}, .default_command = std::string("clang++")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");

    auto header = workspace.file_table.intern(tmp.path("src/x.h"));
    auto same_stem = workspace.file_table.intern(tmp.path("src/x.cpp"));
    auto first = workspace.file_table.intern(tmp.path("src/aaa.cpp"));
    auto other = workspace.file_table.intern(tmp.path("src/other.cpp"));
    auto plain = workspace.file_table.intern(tmp.path("src/plain.c"));
    EXPECT_EQ(command_lender(workspace, header)->unit, same_stem);
    EXPECT_EQ(command_lender(workspace, other)->unit, first);
    EXPECT_FALSE(command_lender(workspace, plain).has_value());
};

TEST_CASE(LenderSearchDir) {
    /// A header under a command's header search directory borrows that
    /// command — the entry that searches there, not the unit's first —
    /// over the unit closest by path; a source there borrows the closest.
    TempDir tmp;
    tmp.touch("include/api/new.h", "");
    Workspace workspace;
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    auto add = [&](llvm::StringRef file, llvm::StringRef flags) {
        auto command = std::format("clang++ {} {}", flags, tmp.path(file));
        return *workspace.cdb.add_command(tmp.root.str(), tmp.path(file), llvm::StringRef(command));
    };
    add("zzz/lib.cpp", "");
    auto searching = add("zzz/lib.cpp", "-Iinclude");
    auto near = add("include/near.cpp", "");

    auto header = workspace.file_table.intern(tmp.path("include/api/new.h"));
    auto lender = command_lender(workspace, header);
    ASSERT_TRUE(lender.has_value());
    EXPECT_EQ(lender->unit, searching.file);
    EXPECT_EQ(lender->config, searching.config);

    auto source = workspace.file_table.intern(tmp.path("include/api/new.cpp"));
    EXPECT_EQ(command_lender(workspace, source)->unit, near.file);
};

TEST_CASE(LenderIgnoresCommandless) {
    /// A member a rule claims with a default command that is no compile
    /// command lends nothing.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    tmp.touch("src/b.cpp", "");
    Workspace workspace;
    workspace.config.rules.push_back(ConfigRule{.default_command = std::string("ccache")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    ASSERT_EQ(workspace.build.members().size(), 2u);
    auto header = workspace.file_table.intern(tmp.path("src/new.h"));
    EXPECT_FALSE(command_lender(workspace, header).has_value());
};

};  // TEST_SUITE(Hosting)

}  // namespace

}  // namespace clice::testing
