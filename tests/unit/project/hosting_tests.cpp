#include <format>

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "project/hosting.h"
#include "project/project.h"

namespace clice::testing {

namespace {

ZEST_SUITE(Hosting) {

ZEST_CASE(SourcePriorityBeatsProximity) {
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

    FileTable files;

    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"lib/**"}, .compile_commands = {"lib/cmake"}});
    project.config.rules.push_back(ConfigRule{.compile_commands = {"cmake"}});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    for(auto source: project.build.declared_sources()) {
        project.cdb.load(source);
    }

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("lib/x.h")));
    auto near = project.file_table.intern(Spelling::absolute(tmp.path("lib/near.cpp")));
    auto far = project.file_table.intern(Spelling::absolute(tmp.path("src/far.cpp")));
    project.dep_graph.set_includes(near, 0, {{header}});
    project.dep_graph.set_includes(far, 0, {{header}});
    project.dep_graph.build_reverse_map();

    auto ranked = ranked_hosts(project, header);
    ZASSERT(ranked.size() == 2u);
    ZEXPECT(ranked[0] == far);
    ZEXPECT(ranked[1] == near);
    auto host = default_host(project, header);
    ZASSERT(host);
    ZEXPECT(host->file == far);
    ZEXPECT(host->chain.back() == header);
};

ZEST_CASE(ProximityWithinSource) {
    /// Same database: the unit sharing the header's stem wins, then the one
    /// in its directory; a unit the build does not compile is no host.
    TempDir tmp;
    tmp.touch("src/x.h", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(ConfigRule{
        .patterns = {"src/**", "other/**"},
        .default_command = std::string("clang++")
    });
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/x.h")));
    auto same_stem = project.file_table.intern(Spelling::absolute(tmp.path("other/x.cpp")));
    auto same_dir = project.file_table.intern(Spelling::absolute(tmp.path("src/y.cpp")));
    auto elsewhere = project.file_table.intern(Spelling::absolute(tmp.path("other/z.cpp")));
    auto not_compiled = project.file_table.intern(Spelling::absolute(tmp.path("skip/w.cpp")));
    for(auto host: {same_stem, same_dir, elsewhere, not_compiled}) {
        project.dep_graph.set_includes(host, 0, {{header}});
    }
    project.dep_graph.build_reverse_map();

    auto ranked = ranked_hosts(project, header);
    ZASSERT(ranked.size() == 3u);
    ZEXPECT(ranked[0] == same_stem);
    ZEXPECT(ranked[1] == same_dir);
    ZEXPECT(ranked[2] == elsewhere);

    /// Equal scores fall back to path order, so the ranking is stable.
    auto first = project.file_table.intern(Spelling::absolute(tmp.path("other/aaa.cpp")));
    auto second = project.file_table.intern(Spelling::absolute(tmp.path("other/bbb.cpp")));
    project.dep_graph.set_includes(second, 0, {{header}});
    project.dep_graph.set_includes(first, 0, {{header}});
    project.dep_graph.build_reverse_map();
    ranked = ranked_hosts(project, header);
    ZASSERT(ranked.size() == 5u);
    ZEXPECT(ranked[2] == first);
    ZEXPECT(ranked[3] == second);
    ZEXPECT(ranked[4] == elsewhere);
};

ZEST_CASE(ProximityByDirectories) {
    /// Closeness counts whole directories: `a/b/x.h` is as near the units
    /// of `a/` as `a/bc/` is, and those go no deeper.
    TempDir tmp;
    tmp.touch("a/b/x.h", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"a/**"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("a/b/x.h")));
    auto parent = project.file_table.intern(Spelling::absolute(tmp.path("a/zz.cpp")));
    auto cousin = project.file_table.intern(Spelling::absolute(tmp.path("a/bc/z.cpp")));
    for(auto host: {parent, cousin}) {
        project.dep_graph.set_includes(host, 0, {{header}});
    }
    project.dep_graph.build_reverse_map();

    auto ranked = ranked_hosts(project, header);
    ZASSERT(ranked.size() == 2u);
    ZEXPECT(ranked[0] == parent);
    ZEXPECT(ranked[1] == cousin);
};

ZEST_CASE(HostsMatchLanguage) {
    /// A C unit never hosts a C++ header; an ambiguous `.h` takes any host.
    TempDir tmp;
    tmp.touch("shared/types.hpp", "");
    tmp.touch("shared/plain.h", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"c/**"}, .default_command = std::string("clang")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto hpp = project.file_table.intern(Spelling::absolute(tmp.path("shared/types.hpp")));
    auto plain = project.file_table.intern(Spelling::absolute(tmp.path("shared/plain.h")));
    auto impl = project.file_table.intern(Spelling::absolute(tmp.path("c/impl.c")));
    project.dep_graph.set_includes(impl, 0, {{hpp}, {plain}});
    project.dep_graph.build_reverse_map();

    ZEXPECT(ranked_hosts(project, hpp).empty());
    ZEXPECT(ranked_hosts(project, plain) == llvm::SmallVector<Fid>{impl});

    /// A source borrows only its own language: a `.cl` or a `.m` next to
    /// the C unit would compile as C under its command.
    auto kernel_cl = project.file_table.intern(Spelling::absolute(tmp.path("c/kernel.cl")));
    auto objc = project.file_table.intern(Spelling::absolute(tmp.path("c/new.m")));
    ZEXPECT(!command_lender(project, kernel_cl).has_value());
    ZEXPECT(!command_lender(project, objc).has_value());

    /// An Objective-C++ unit is C++ with more: it hosts a C++ header.
    tmp.touch("mac/impl.mm", "");
    project.config.rules.push_back(
        ConfigRule{.patterns = {"mac/**"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    project.commands_epoch += 1;
    auto impl_mm = project.file_table.intern(Spelling::absolute(tmp.path("mac/impl.mm")));
    project.dep_graph.set_includes(impl_mm, 0, {{hpp}});
    project.dep_graph.build_reverse_map();
    ZEXPECT(ranked_hosts(project, hpp) == llvm::SmallVector<Fid>{impl_mm});

    /// A CUDA unit is C++ with device code: it hosts a C++ header.
    tmp.touch("gpu/kernel.cu", "");
    project.config.rules.push_back(
        ConfigRule{.patterns = {"gpu/**"}, .default_command = std::string("clang++ -x cuda")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    project.commands_epoch += 1;
    auto kernel = project.file_table.intern(Spelling::absolute(tmp.path("gpu/kernel.cu")));
    project.dep_graph.set_includes(kernel, 0, {{hpp}});
    project.dep_graph.build_reverse_map();
    ZEXPECT(ranked_hosts(project, hpp) == (llvm::SmallVector<Fid>{kernel, impl_mm}));

    /// Only headers get that latitude: a C++ source borrowing the CUDA
    /// command would compile as CUDA.
    auto gpu_header = project.file_table.intern(Spelling::absolute(tmp.path("gpu/new.hpp")));
    auto gpu_source = project.file_table.intern(Spelling::absolute(tmp.path("gpu/new.cpp")));
    ZEXPECT(command_lender(project, gpu_header)->unit == kernel);
    ZEXPECT(!command_lender(project, gpu_source).has_value());

    /// A host offers only the commands that fit the header: with a C entry
    /// first and a C++ one second, a `.hpp` sees the second alone.
    tmp.touch("dual/impl.c", "");
    auto dual = project.file_table.intern(Spelling::absolute(tmp.path("dual/impl.c")));
    auto dual_hpp = project.file_table.intern(Spelling::absolute(tmp.path("dual/x.hpp")));
    auto c_command = std::format("clang -x c {}", tmp.path("dual/impl.c"));
    auto cxx_command = std::format("clang++ -x c++ {}", tmp.path("dual/impl.c"));
    project.cdb.add_command(tmp.root.str(), tmp.path("dual/impl.c"), llvm::StringRef(c_command));
    auto cxx = *project.cdb.add_command(tmp.root.str(),
                                        tmp.path("dual/impl.c"),
                                        llvm::StringRef(cxx_command));
    project.dep_graph.set_includes(dual, 0, {{dual_hpp}});
    project.dep_graph.build_reverse_map();
    auto fitting = host_commands(project, dual_hpp, dual);
    ZASSERT(fitting.size() == 1u);
    ZEXPECT(fitting.front().config == cxx.config);
    ZEXPECT(ranked_hosts(project, dual_hpp) == llvm::SmallVector<Fid>{dual});
};

ZEST_CASE(LenderSibling) {
    /// A file without a command borrows from a unit in its directory, the
    /// one sharing its stem before the first by name; a `.c` only from a C
    /// unit, and nothing when the build has none.
    TempDir tmp;
    tmp.touch("src/aaa.cpp", "");
    tmp.touch("src/x.cpp", "");
    tmp.touch("src/x.h", "");
    tmp.touch("src/new.cpp", "");
    tmp.touch("src/plain.c", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"src/*.cpp"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/x.h")));
    auto same_stem = project.file_table.intern(Spelling::absolute(tmp.path("src/x.cpp")));
    auto first = project.file_table.intern(Spelling::absolute(tmp.path("src/aaa.cpp")));
    auto other = project.file_table.intern(Spelling::absolute(tmp.path("src/other.cpp")));
    auto plain = project.file_table.intern(Spelling::absolute(tmp.path("src/plain.c")));
    ZEXPECT(command_lender(project, header)->unit == same_stem);
    ZEXPECT(command_lender(project, other)->unit == first);
    ZEXPECT(!command_lender(project, plain).has_value());
};

ZEST_CASE(LenderCxxDriverC) {
    /// A `.c` borrows from a `.c` unit its C++ driver compiles as C++.
    TempDir tmp;
    tmp.touch("lib/a.c", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"lib/*.c"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto borrower = project.file_table.intern(Spelling::absolute(tmp.path("lib/new.c")));
    auto unit = project.file_table.intern(Spelling::absolute(tmp.path("lib/a.c")));
    ZEXPECT(command_lender(project, borrower)->unit == unit);
};

ZEST_CASE(LenderByDirectories) {
    /// Away from every unit's directory, a file borrows from the unit the
    /// fewest directories off, not the one sharing the longest spelling.
    TempDir tmp;
    tmp.touch("a/zz.cpp", "");
    tmp.touch("a/bc/z.cpp", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(
        ConfigRule{.patterns = {"a/**"}, .default_command = std::string("clang++")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");

    auto borrower = project.file_table.intern(Spelling::absolute(tmp.path("a/b/new.cpp")));
    auto parent = project.file_table.intern(Spelling::absolute(tmp.path("a/zz.cpp")));
    ZEXPECT(command_lender(project, borrower)->unit == parent);
};

ZEST_CASE(LenderSearchDir) {
    /// A header under a command's header search directory borrows that
    /// command — the entry that searches there, not the unit's first —
    /// over the unit closest by path; a source there borrows the closest.
    TempDir tmp;
    tmp.touch("include/api/new.h", "");
    FileTable files;
    Project project{files};
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    auto add = [&](llvm::StringRef file, llvm::StringRef flags) {
        tmp.touch(file, "");
        auto command = std::format("clang++ {} {}", flags, tmp.path(file));
        return *project.cdb.add_command(tmp.root.str(), tmp.path(file), llvm::StringRef(command));
    };
    add("zzz/lib.cpp", "");
    auto searching = add("zzz/lib.cpp", "-Iinclude");
    auto near = add("include/near.cpp", "");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("include/api/new.h")));
    auto lender = command_lender(project, header);
    ZASSERT(lender);
    ZEXPECT(lender->unit == searching.file);
    ZEXPECT(lender->config == searching.config);

    auto source = project.file_table.intern(Spelling::absolute(tmp.path("include/api/new.cpp")));
    ZEXPECT(command_lender(project, source)->unit == near.file);
};

ZEST_CASE(LenderIgnoresCommandless) {
    /// A member a rule claims with a default command that is no compile
    /// command lends nothing.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    tmp.touch("src/b.cpp", "");
    FileTable files;
    Project project{files};
    project.config.rules.push_back(ConfigRule{.default_command = std::string("ccache")});
    project.config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    project.build.reset_active("");
    ZASSERT(project.build.members().size() == 2u);
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/new.h")));
    ZEXPECT(!command_lender(project, header).has_value());
};

};  // ZEST_SUITE(Hosting)

}  // namespace

}  // namespace clice::testing
