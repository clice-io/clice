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

    /// OpenCL is C-derived but not C: a `.cl` borrows no C command, nor
    /// another specialized language's.
    tmp.touch("asm/boot.s", "");
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"asm/**"}, .default_command = std::string("clang -x assembler")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    workspace.commands_epoch += 1;
    auto kernel_cl = workspace.file_table.intern(tmp.path("c/kernel.cl"));
    auto asm_cl = workspace.file_table.intern(tmp.path("asm/kernel.cl"));
    EXPECT_FALSE(command_lender(workspace, kernel_cl).has_value());
    EXPECT_FALSE(command_lender(workspace, asm_cl).has_value());

    /// The same specialized language does lend.
    tmp.touch("cl/lib.cl", "");
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"cl/**"}, .default_command = std::string("clang -x cl")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    workspace.commands_epoch += 1;
    auto lib_cl = workspace.file_table.intern(tmp.path("cl/lib.cl"));
    auto new_cl = workspace.file_table.intern(tmp.path("cl/new.cl"));
    EXPECT_EQ(command_lender(workspace, new_cl)->unit, lib_cl);

    /// A CUDA unit is C++ with device code: it hosts a C++ header.
    tmp.touch("gpu/kernel.cu", "");
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"gpu/**"}, .default_command = std::string("clang++ -x cuda")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    workspace.commands_epoch += 1;
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

    /// HIP is the same family.
    tmp.touch("hip/kernel.hip", "");
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"hip/**"}, .default_command = std::string("clang++ -x hip")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    workspace.commands_epoch += 1;
    auto hip = workspace.file_table.intern(tmp.path("hip/kernel.hip"));
    auto hip_header = workspace.file_table.intern(tmp.path("hip/new.hpp"));
    auto hip_cuda = workspace.file_table.intern(tmp.path("hip/new.cu"));
    EXPECT_EQ(command_lender(workspace, hip_header)->unit, hip);
    /// A `.cu` next to the HIP unit borrows the CUDA one further away.
    EXPECT_EQ(command_lender(workspace, hip_cuda)->unit, kernel);

    /// A host offers only the commands that fit the header: with a C entry
    /// first and a C++ one second, a `.hpp` sees the second alone.
    tmp.touch("dual/impl.c", "");
    auto dual = workspace.file_table.intern(tmp.path("dual/impl.c"));
    auto dual_hpp = workspace.file_table.intern(tmp.path("dual/x.hpp"));
    auto c_command = std::format("clang -x c {}", tmp.path("dual/impl.c"));
    auto cxx_command = std::format("clang++ -x c++ {}", tmp.path("dual/impl.c"));
    workspace.cdb.add_command(tmp.root.str(), tmp.path("dual/impl.c"), llvm::StringRef(c_command));
    auto cxx = *workspace.cdb.add_command(tmp.root.str(),
                                          tmp.path("dual/impl.c"),
                                          llvm::StringRef(cxx_command));
    workspace.dep_graph.set_includes(dual, 0, {{dual_hpp}});
    workspace.dep_graph.build_reverse_map();
    auto fitting = host_commands(workspace, dual_hpp, dual);
    ASSERT_EQ(fitting.size(), 1u);
    EXPECT_EQ(fitting.front().config, cxx.config);
    EXPECT_EQ(ranked_hosts(workspace, dual_hpp), llvm::SmallVector<Fid>{dual});
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
        tmp.touch(file, "");
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

TEST_CASE(LenderSkipsDeleted) {
    /// A listed unit gone from disk lends nothing.
    TempDir tmp;
    tmp.touch("src/x.h", "");
    tmp.touch("far/lib.cpp", "");
    Workspace workspace;
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    for(auto file: {"src/gone.cpp", "far/lib.cpp"}) {
        auto command = std::format("clang++ {}", tmp.path(file));
        workspace.cdb.add_command(tmp.root.str(), tmp.path(file), llvm::StringRef(command));
    }
    auto header = workspace.file_table.intern(tmp.path("src/x.h"));
    auto lib = workspace.file_table.intern(tmp.path("far/lib.cpp"));
    auto gone = workspace.file_table.intern(tmp.path("src/gone.cpp"));
    EXPECT_EQ(command_lender(workspace, header)->unit, lib);
    EXPECT_TRUE(workspace.lenders.missing.contains(gone));

    /// Back on disk, it lends once the lender set is known to have changed.
    tmp.touch("src/gone.cpp", "");
    workspace.commands_epoch += 1;
    EXPECT_EQ(command_lender(workspace, header)->unit, gone);
};

TEST_CASE(LenderKeepsObjC) {
    /// Objective-C is its own family: a `.m` never borrows a C command,
    /// and a `.mm` unit hosts a C++ header the way a CUDA one does.
    TempDir tmp;
    tmp.touch("src/plain.c", "");
    tmp.touch("src/impl.mm", "");
    tmp.touch("src/x.hpp", "");
    Workspace workspace;
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"src/*.c"}, .default_command = std::string("clang")});
    workspace.config.rules.push_back(
        ConfigRule{.patterns = {"src/*.mm"}, .default_command = std::string("clang++")});
    workspace.config.finalize(tmp.root.str());
    workspace.build.reset_active("");
    auto objc = workspace.file_table.intern(tmp.path("src/new.m"));
    auto impl = workspace.file_table.intern(tmp.path("src/impl.mm"));
    auto hpp = workspace.file_table.intern(tmp.path("src/x.hpp"));
    EXPECT_FALSE(command_lender(workspace, objc).has_value());
    EXPECT_EQ(command_lender(workspace, hpp)->unit, impl);
    workspace.dep_graph.set_includes(impl, 0, {{hpp}});
    workspace.dep_graph.build_reverse_map();
    EXPECT_EQ(ranked_hosts(workspace, hpp), llvm::SmallVector<Fid>{impl});
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
