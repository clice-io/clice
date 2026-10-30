module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.table;
import :compile.diagnostic;
import :feature.diagnostics;
import :index.project_index;
import :index.tu_manifest;
import :project.hosting;
import :project.project;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :unittests.utils;

namespace clice::testing {

namespace {

/// A project whose `*.cpp` under `tmp` compile with a default command.
struct TreeProject {
    TempDir tmp;
    FileTable files;
    Project project{files};

    TreeProject() {
        project.settings.rules.push_back(
            Rule{.patterns = {"**/*.cpp"}, .default_command = std::string("clang++")});
        project.root = CanonicalPath(Spelling::absolute(tmp.root));
        project.settings.finalize(project.root);
        project.build.reset_active("");
    }

    /// Write the file and name it.
    Fid write(llvm::StringRef path, llvm::StringRef content) {
        tmp.touch(path, content);
        return project.file_table.intern(Spelling::absolute(tmp.path(path)));
    }

    /// The version of the file holding `content`.
    std::uint32_t version(Fid file, llvm::StringRef content) {
        return project.file_table.intern_version(file, llvm::xxh3_64bits(content)).raw;
    }

    /// The include tree `host`'s first command takes.
    TreeRequest tree(Fid host) {
        return tree_request(project, host, project.build.commands(host).front());
    }

    /// The host find_host finds for `header` with nothing pinned.
    std::optional<Host> host(Fid header) {
        return find_host(project, header, nullptr).host;
    }

    /// include_chains under `host`'s first command.
    std::optional<llvm::SmallVector<Host>> chains(Fid host, Fid header) {
        return include_chains(project, tree(host).key, header);
    }
};

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
    project.settings.rules.push_back(
        Rule{.patterns = {"lib/**"}, .compile_commands = {"lib/cmake"}});
    project.settings.rules.push_back(Rule{.compile_commands = {"cmake"}});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    /// The search waits for the first one's include tree.
    auto search = find_host(project, header, nullptr);
    ZEXPECT(!search.host.has_value());
    ZASSERT(search.missing);
    ZEXPECT(search.missing->key.first == far);
};

ZEST_CASE(ProximityWithinSource) {
    /// Same database: the unit sharing the header's stem wins, then the one
    /// in its directory; a unit the build does not compile is no host.
    TempDir tmp;
    tmp.touch("src/x.h", "");
    FileTable files;
    Project project{files};
    project.settings.rules.push_back(Rule{
        .patterns = {"src/**", "other/**"},
        .default_command = std::string("clang++")
    });
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"a/**"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"c/**"}, .default_command = std::string("clang")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"mac/**"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    project.commands_epoch += 1;
    auto impl_mm = project.file_table.intern(Spelling::absolute(tmp.path("mac/impl.mm")));
    project.dep_graph.set_includes(impl_mm, 0, {{hpp}});
    project.dep_graph.build_reverse_map();
    ZEXPECT(ranked_hosts(project, hpp) == llvm::SmallVector<Fid>{impl_mm});

    /// A CUDA unit is C++ with device code: it hosts a C++ header.
    tmp.touch("gpu/kernel.cu", "");
    project.settings.rules.push_back(
        Rule{.patterns = {"gpu/**"}, .default_command = std::string("clang++ -x cuda")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.cdb.add_command(tmp.root, tmp.path("dual/impl.c"), llvm::StringRef(c_command));
    auto cxx =
        *project.cdb.add_command(tmp.root, tmp.path("dual/impl.c"), llvm::StringRef(cxx_command));
    project.dep_graph.set_includes(dual, 0, {{dual_hpp}});
    project.dep_graph.build_reverse_map();
    auto fitting = host_commands(project, dual_hpp, dual);
    ZASSERT(fitting.size() == 1u);
    ZEXPECT(fitting.front().command == cxx.command);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"src/*.cpp"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"lib/*.c"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.settings.rules.push_back(
        Rule{.patterns = {"a/**"}, .default_command = std::string("clang++")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
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
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto add = [&](llvm::StringRef file, llvm::StringRef flags) {
        tmp.touch(file, "");
        auto command = std::format("clang++ {} {}", flags, tmp.path(file));
        return *project.cdb.add_command(tmp.root, tmp.path(file), llvm::StringRef(command));
    };
    add("zzz/lib.cpp", "");
    auto searching = add("zzz/lib.cpp", "-Iinclude");
    auto near = add("include/near.cpp", "");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("include/api/new.h")));
    auto lender = command_lender(project, header);
    ZASSERT(lender);
    ZEXPECT(lender->unit == searching.file);
    ZEXPECT(lender->command == searching.command);

    auto source = project.file_table.intern(Spelling::absolute(tmp.path("include/api/new.cpp")));
    ZEXPECT(command_lender(project, source)->unit == near.file);
};

#ifndef _WIN32
ZEST_CASE(LenderSearchDirSymlink) {
    /// A command's search directory reaching the header through a symlink
    /// lends it that command, over the unit beside the header's real path.
    TempDir tmp;
    tmp.touch("vendor/real/orphan.h", "");
    tmp.touch("a/main.cpp", "");
    ZASSERT(::symlink(tmp.path("vendor/real").c_str(), tmp.path("a/inc").c_str()) == 0);
    FileTable files;
    Project project{files};
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto add = [&](llvm::StringRef file, llvm::StringRef flags) {
        tmp.touch(file, "");
        auto command = std::format("clang++ {} {}", flags, tmp.path(file));
        return *project.cdb.add_command(tmp.root, tmp.path(file), llvm::StringRef(command));
    };
    auto searching = add("a/main.cpp", std::format("-I{}", tmp.path("a/inc")));
    add("vendor/b.cpp", "");

    auto header = project.file_table.intern(Spelling::absolute(tmp.path("vendor/real/orphan.h")));
    auto lender = command_lender(project, header);
    ZASSERT(lender);
    ZEXPECT(lender->unit == searching.file);
};
#endif

ZEST_CASE(LenderIgnoresCommandless) {
    /// A member a rule covers with a default command that is no compile
    /// command lends nothing.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    tmp.touch("src/b.cpp", "");
    FileTable files;
    Project project{files};
    project.settings.rules.push_back(Rule{.default_command = std::string("ccache")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    ZASSERT(project.build.members().size() == 2u);
    auto header = project.file_table.intern(Spelling::absolute(tmp.path("src/new.h")));
    ZEXPECT(!command_lender(project, header).has_value());
};

ZEST_CASE(LenderAcrossModules) {
    /// A C++ source and a C++ module unit lend to each other: the first
    /// module interface among `.cpp` units borrows their command, and an
    /// implementation unit its interface's.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    tmp.touch("mods/m.cppm", "");
    FileTable files;
    Project project{files};
    project.settings.rules.push_back(
        Rule{.patterns = {"src/*.cpp"}, .default_command = std::string("clang++")});
    project.settings.rules.push_back(
        Rule{.patterns = {"mods/*.cppm"}, .default_command = std::string("clang++ -std=c++20")});
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");

    auto intern = [&](llvm::StringRef file) {
        return project.file_table.intern(Spelling::absolute(tmp.path(file)));
    };
    ZEXPECT(command_lender(project, intern("src/first.cppm"))->unit == intern("src/a.cpp"));
    ZEXPECT(command_lender(project, intern("mods/m_impl.cpp"))->unit == intern("mods/m.cppm"));
};

ZEST_CASE(ProvisionalMembers) {
    /// A recorded file is a member under its lender's command while it has
    /// a lender and no host; it lends to no one, and an entry listing it
    /// takes over from the record.
    TempDir tmp;
    FileTable files;
    Project project{files};
    project.root = CanonicalPath(Spelling::absolute(tmp.root));
    project.settings.finalize(project.root);
    project.build.reset_active("");
    auto add = [&](llvm::StringRef driver, llvm::StringRef file) {
        tmp.touch(file, "");
        auto command = std::format("{} {}", driver, tmp.path(file));
        return *project.cdb.add_command(tmp.root, tmp.path(file), llvm::StringRef(command));
    };
    auto intern = [&](llvm::StringRef file) {
        tmp.touch(file, "");
        return project.file_table.intern(Spelling::absolute(tmp.path(file)));
    };
    auto unit = add("clang++", "src/b.cpp");

    auto saved = intern("src/a_new.cpp");
    ZEXPECT(project.build.record(saved));
    ZEXPECT(!project.build.record(saved));
    ZEXPECT(project.refresh_provisional().added == llvm::SmallVector<Fid>{saved});
    ZEXPECT(project.build.unit(saved));
    auto commands = project.build.commands(saved);
    ZASSERT(commands.size() == 1u);
    ZEXPECT(commands.front().source == CommandSource::Inferred);
    ZEXPECT(project.build.members().back() == saved);

    /// `a_new.cpp` sorts first in the directory, yet `b.cpp` lends: a
    /// provisional member lends to no one.
    project.commands_epoch += 1;
    ZEXPECT(command_lender(project, intern("src/other.cpp"))->unit == unit.file);

    /// A unit sorting before `b.cpp` lends instead: the member's command
    /// changes with it.
    add("clang++ -DSIBLING", "src/a.cpp");
    project.commands_epoch += 1;
    ZEXPECT(project.refresh_provisional().changed == llvm::SmallVector<Fid>{saved});

    /// No C unit lends to a `.c` yet: recorded, not a member until one does.
    auto c_file = intern("src/new.c");
    project.build.record(c_file);
    ZEXPECT(project.refresh_provisional().empty());
    ZEXPECT(!project.build.unit(c_file));
    add("clang", "src/legacy.c");
    project.commands_epoch += 1;
    ZEXPECT(project.refresh_provisional().added == llvm::SmallVector<Fid>{c_file});

    /// A file a unit includes keeps compiling in its includer's context.
    auto included = intern("src/table.cpp");
    project.build.record(included);
    project.dep_graph.set_includes(unit.file, 0, {{included}});
    project.dep_graph.build_reverse_map();
    ZEXPECT(project.refresh_provisional().empty());
    ZEXPECT(!project.build.unit(included));

    /// Only a declared unit's include keeps a file out: one a provisional
    /// member includes joins too, whichever of the two sorts first.
    auto inner = intern("src/c_inner.cpp");
    auto outer = intern("src/c_outer.cpp");
    project.build.record(inner);
    project.build.record(outer);
    project.dep_graph.set_includes(outer, 0, {{inner}});
    ZEXPECT(project.refresh_provisional().added == (llvm::SmallVector<Fid>{inner, outer}));

    /// An entry listing the file takes over; the record goes.
    add("clang++", "src/a_new.cpp");
    ZEXPECT(project.refresh_provisional().empty());
    ZEXPECT(!project.build.recorded().contains(saved));
    ZEXPECT(project.build.commands(saved).front().source == CommandSource::CDBExact);
};

ZEST_CASE(ChainsFollowTree) {
    /// The unit enters `bar.h` through `foo.h`; its own include of it
    /// comes later and finds the guard: the chain is the one the compile
    /// took, not the shortest.
    TreeProject p;
    llvm::StringRef main_text = "#include \"foo.h\"\n#include \"bar.h\"\n";
    llvm::StringRef foo_text = "#pragma once\n#include \"bar.h\"\n";
    llvm::StringRef bar_text = "#pragma once\nint b;\n";
    auto main = p.write("main.cpp", main_text);
    auto foo = p.write("foo.h", foo_text);
    auto bar = p.write("bar.h", bar_text);
    p.project.dep_graph.set_includes(main, 0, {{foo}, {bar}});
    p.project.dep_graph.set_includes(foo, 0, {{bar}});
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(foo, foo_text), .line = 1},
        {.file = p.version(bar, bar_text), .parent = 0, .line = 2},
        {.file = p.version(bar, bar_text), .line = 2, .skipped = true},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    auto host = p.host(bar);
    ZASSERT(host);
    ZEXPECT(host->chain == std::vector<Fid>{main, foo, bar});
    ZEXPECT(host->lines == llvm::SmallVector<std::uint32_t>{1, 2});
    ZEXPECT(count_occurrences(p.project, main, bar) == 1u);
};

ZEST_CASE(TreeRulesOutHost) {
    /// The scan resolved `shared.h`'s include under b.cpp's directories;
    /// a.cpp's compile enters another `config.h`, so it lends no context.
    TreeProject p;
    llvm::StringRef a_text = "#include \"shared.h\"\n";
    llvm::StringRef shared_text = "#pragma once\n#include <config.h>\n";
    auto a = p.write("a.cpp", a_text);
    auto b = p.write("b.cpp", "#include \"shared.h\"\n");
    auto shared = p.write("common/shared.h", shared_text);
    auto config_a = p.write("config_a/config.h", "");
    auto config_b = p.write("config_b/config.h", "");
    p.project.dep_graph.set_includes(a, 0, {{shared}});
    p.project.dep_graph.set_includes(b, 0, {{shared}});
    p.project.dep_graph.set_includes(shared, 0, {{config_b}});
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(a, a_text)};
    manifest.nodes = {
        {.file = p.version(shared, shared_text), .line = 1},
        {.file = p.version(config_a, ""), .parent = 0, .line = 2},
    };
    p.project.index.apply_manifest(a, std::move(manifest));

    ZEXPECT(count_occurrences(p.project, a, config_b) == 0u);
    /// b.cpp's compile may enter it: the search waits for its tree.
    auto search = find_host(p.project, config_b, nullptr);
    ZASSERT(search.missing);
    ZEXPECT(search.missing->key.first == b);
    llvm::StringRef b_text = "#include \"shared.h\"\n";
    p.project.hosting.trees.commit(
        p.tree(b).key,
        {
            .root = VersionID{p.version(b, b_text)},
            .nodes =
                {
                              {.file = p.version(shared, shared_text), .line = 1},
                              {.file = p.version(config_b, ""), .parent = 0, .line = 2},
                              }
    },
        {});
    auto host = p.host(config_b);
    ZASSERT(host);
    ZEXPECT(host->file == b);
    ZEXPECT(host->lines == llvm::SmallVector<std::uint32_t>{1, 2});

    /// Once shared.h changes, the tree confirms nothing on its way.
    p.write("common/shared.h", "#pragma once\n#include <config.h>\nint more;\n");
    p.project.file_table.disk.end_turn();
    ZEXPECT(!p.chains(a, config_b).has_value());
};

ZEST_CASE(ChainsInCompileOrder) {
    /// Node numbers follow includers, not the compile: a later directive
    /// of the unit numbered before a nested one still comes after it.
    TreeProject p;
    llvm::StringRef main_text = "#include \"mid.h\"\n#define AGAIN\n#include \"list.def\"\n";
    llvm::StringRef mid_text = "#include \"list.def\"\n";
    auto main = p.write("main.cpp", main_text);
    auto mid = p.write("mid.h", mid_text);
    auto list = p.write("list.def", "X(a)\n");

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(list, "X(a)\n"), .line = 3},
        {.file = p.version(mid, mid_text), .line = 1},
        {.file = p.version(list, "X(a)\n"), .parent = 1, .line = 1},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    auto found = p.chains(main, list);
    ZASSERT(found);
    ZASSERT(found->size() == 2u);
    ZEXPECT((*found)[0].lines == llvm::SmallVector<std::uint32_t>{1, 1});
    ZEXPECT((*found)[1].lines == llvm::SmallVector<std::uint32_t>{3});
};

ZEST_CASE(ForcedIncludeEnters) {
    /// The compile enters the header through a file its command forces in,
    /// ahead of the unit's own include of it: the chain starts there.
    TreeProject p;
    llvm::StringRef main_text = R"(#include "t.h")";
    llvm::StringRef common_text = R"(#pragma once
#include "t.h")";
    auto main = p.write("main.cpp", main_text);
    auto common = p.write("common.h", common_text);
    auto t = p.write("t.h", "#pragma once\n");
    p.project.dep_graph.set_includes(main, 0, {{t}});
    p.project.dep_graph.set_includes(common, 0, {{t}});
    p.project.dep_graph.add_forced_include(main, common);
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(common, common_text), .line = 300},
        {.file = p.version(t, "#pragma once\n"), .parent = 0, .line = 2},
        {.file = p.version(t, "#pragma once\n"), .line = 1, .skipped = true},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    auto found = p.chains(main, t);
    ZASSERT(found);
    ZASSERT(found->size() == 1u);
    ZEXPECT(found->front().chain == std::vector<Fid>{common, t});
    ZEXPECT(found->front().lines == llvm::SmallVector<std::uint32_t>{2});
};

ZEST_CASE(PrecompiledHeaderHidesWay) {
    /// The forced file is a precompiled header's: the compile took it, a.h
    /// and t.h from the PCH, and the manifest's tree shows only the unit's
    /// include of a.h, skipped. It cannot tell; a preprocess run, which
    /// reads the forced file as text, does.
    TreeProject p;
    llvm::StringRef main_text = R"(#include "a.h")";
    llvm::StringRef a_text = R"(#pragma once
#include "t.h")";
    auto main = p.write("main.cpp", main_text);
    auto pch = p.write("pch.h", R"(#include "a.h")");
    auto a = p.write("a.h", a_text);
    auto t = p.write("t.h", "#pragma once\n");
    p.project.dep_graph.set_includes(main, 0, {{a}});
    p.project.dep_graph.set_includes(pch, 0, {{a}});
    p.project.dep_graph.set_includes(a, 0, {{t}});
    p.project.dep_graph.add_forced_include(main, pch);
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(a, a_text), .line = 1, .skipped = true},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    ZEXPECT(!p.chains(main, t).has_value());
    p.project.hosting.trees.commit(
        p.tree(main).key,
        {
            .root = VersionID{p.version(main, main_text)},
            .nodes =
                {
                              {.file = p.version(pch, R"(#include "a.h")"), .line = 300},
                              {.file = p.version(a, a_text), .parent = 0, .line = 1},
                              {.file = p.version(t, "#pragma once\n"), .parent = 1, .line = 2},
                              {.file = p.version(a, a_text), .line = 1, .skipped = true},
                              }
    },
        {});
    auto host = p.host(t);
    ZASSERT(host);
    ZEXPECT(host->file == main);
    ZEXPECT(host->chain == std::vector<Fid>{pch, a, t});
    ZEXPECT(host->lines == llvm::SmallVector<std::uint32_t>{1, 2});
};

ZEST_CASE(ForcedOnlyHost) {
    /// No unit includes t.h; the command of main.cpp forces in pch.h,
    /// which does: main.cpp hosts it, and pch.h itself, once its tree says
    /// so.
    TreeProject p;
    llvm::StringRef main_text = "int x;\n";
    llvm::StringRef pch_text = R"(#include "t.h")";
    auto main = p.write("main.cpp", main_text);
    auto pch = p.write("pch.h", pch_text);
    auto t = p.write("t.h", "#pragma once\n");
    p.project.dep_graph.set_includes(pch, 0, {{t}});
    p.project.dep_graph.add_forced_include(main, pch);
    p.project.dep_graph.build_reverse_map();

    ZEXPECT(!p.host(t).has_value());
    ZEXPECT(!count_occurrences(p.project, main, t).has_value());
    p.project.hosting.trees.commit(
        p.tree(main).key,
        {
            .root = VersionID{p.version(main, main_text)},
            .nodes =
                {
                              {.file = p.version(pch, pch_text), .line = 1},
                              {.file = p.version(t, "#pragma once\n"), .parent = 0, .line = 1},
                              }
    },
        {});
    auto host = p.host(t);
    ZASSERT(host);
    ZEXPECT(host->chain == std::vector<Fid>{pch, t});
    ZEXPECT(host->lines == llvm::SmallVector<std::uint32_t>{1});
    ZEXPECT(count_occurrences(p.project, main, t) == 1u);
    host = p.host(pch);
    ZASSERT(host);
    ZEXPECT(host->chain == std::vector<Fid>{pch});
    ZEXPECT(count_occurrences(p.project, main, pch) == 1u);
};

ZEST_CASE(ForcedGainsInclude) {
    /// The forced file gained the include of t.h since the tree was taken:
    /// the tree tells nothing about t.h, and the search waits for a new one.
    TreeProject p;
    llvm::StringRef main_text = "int x;\n";
    llvm::StringRef old_text = "#pragma once\n";
    auto main = p.write("main.cpp", main_text);
    auto old_version = p.version(p.write("prefix.h", old_text), old_text);
    auto prefix = p.write("prefix.h", R"(#pragma once
#include "t.h")");
    auto t = p.write("t.h", "#pragma once\n");
    p.project.dep_graph.set_includes(prefix, 0, {{t}});
    p.project.dep_graph.add_forced_include(main, prefix);
    p.project.dep_graph.build_reverse_map();
    p.project.file_table.disk.end_turn();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = old_version, .line = 300},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    ZEXPECT(!p.chains(main, t).has_value());
    auto search = find_host(p.project, t, nullptr);
    ZASSERT(search.missing);
    ZEXPECT(search.missing->key.first == main);
};

ZEST_CASE(SkippedAloneRulesOut) {
    /// No precompiled header: b.h, skipped and never entered, shares a.h's
    /// guard, and the unit never compiles its text.
    TreeProject p;
    llvm::StringRef main_text = R"(#include "a.h"
#include "b.h")";
    llvm::StringRef a_text = "#ifndef UTIL_H\n#define UTIL_H\n#endif\n";
    llvm::StringRef b_text = "#ifndef UTIL_H\n#define UTIL_H\nint b;\n#endif\n";
    auto main = p.write("main.cpp", main_text);
    auto a = p.write("a.h", a_text);
    auto b = p.write("b.h", b_text);
    p.project.dep_graph.set_includes(main, 0, {{a}, {b}});
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(a, a_text), .line = 1},
        {.file = p.version(b, b_text), .line = 2, .skipped = true},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    ZEXPECT(!p.host(b).has_value());
};

ZEST_CASE(ContributorHosts) {
    /// A unit whose indexed compile gave the header entries hosts it though
    /// the scan reached the header under another configuration.
    TreeProject p;
    llvm::StringRef b_text = "#include <config.h>\n";
    auto b = p.write("b.cpp", b_text);
    auto config_b = p.write("config_b/config.h", "");
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(b, b_text)};
    manifest.nodes = {
        {.file = p.version(config_b, ""), .line = 1}
    };
    manifest.contributions = {
        {VersionID{p.version(config_b, "")}, 1}
    };
    p.project.index.apply_manifest(b, std::move(manifest));

    auto host = p.host(config_b);
    ZASSERT(host);
    ZEXPECT(host->file == b);
    ZEXPECT(host->lines == llvm::SmallVector<std::uint32_t>{1});
};

ZEST_CASE(StaleTreeWaits) {
    /// A file on the way changed since the manifest's tree was taken: the
    /// search waits for a preprocess run's.
    TreeProject p;
    llvm::StringRef main_text = "#include \"foo.h\"\n#include \"bar.h\"\n";
    auto main = p.write("main.cpp", main_text);
    auto foo = p.write("foo.h", "#pragma once\n");
    auto bar = p.write("bar.h", "int b;\n");
    p.project.dep_graph.set_includes(main, 0, {{foo}, {bar}});
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(foo, "#pragma once\n#include \"bar.h\"\n"), .line = 1},
        {.file = p.version(bar, "int b;\n"), .parent = 0, .line = 2},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    ZEXPECT(!p.chains(main, bar).has_value());
    auto search = find_host(p.project, bar, nullptr);
    ZASSERT(search.missing);
    ZEXPECT(search.missing->key.first == main);
};

ZEST_CASE(AppearedFileWaits) {
    /// The tree was taken while part.cpp was missing, so its compile
    /// entered nothing there; once the file exists the tree cannot tell.
    TreeProject p;
    llvm::StringRef unity_text = "#define UNITY\n#include \"part.cpp\"\n";
    auto unity = p.write("unity.cpp", unity_text);
    auto part = p.project.file_table.intern(Spelling::absolute(p.tmp.path("part.cpp")));
    auto root = VersionID{p.version(unity, unity_text)};
    std::vector<VersionID> absent{p.project.file_table.intern_version(part, 0)};

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = root;
    manifest.absent = absent;
    p.project.index.apply_manifest(unity, std::move(manifest));
    auto before = p.chains(unity, part);
    ZASSERT(before);
    ZEXPECT(before->empty());

    p.write("part.cpp", "int part;\n");
    p.project.file_table.disk.end_turn();
    p.project.dep_graph.set_includes(unity, 0, {{part}});
    p.project.dep_graph.build_reverse_map();
    ZEXPECT(!p.chains(unity, part).has_value());

    /// A preprocess run's tree the same: its trace names the place.
    p.project.index.remove_manifest(unity);
    p.project.hosting.trees.commit(p.tree(unity).key,
                                   {
                                       .root = root
    },
                                   {{.path_id = part, .missing = true}});
    ZEXPECT(!p.chains(unity, part).has_value());
};

ZEST_CASE(RepeatedHeaderChangeStales) {
    /// The header's guard decided its second include: once the header
    /// changes the tree cannot tell; a header included once keeps it.
    TreeProject p;
    llvm::StringRef main_text = "#include \"t.h\"\n#include \"t.h\"\n#include \"u.h\"\n";
    auto main = p.write("main.cpp", main_text);
    auto t = p.write("t.h", "int t;\n");
    auto u = p.write("u.h", "int u;\n");
    p.project.dep_graph.set_includes(main, 0, {{t}, {t}, {u}});
    p.project.dep_graph.build_reverse_map();

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(main, main_text)};
    manifest.nodes = {
        {.file = p.version(t, "#pragma once\nint t;\n"), .line = 1},
        {.file = p.version(t, "#pragma once\nint t;\n"), .line = 2, .skipped = true},
        {.file = p.version(u, "#pragma once\nint u;\n"), .line = 3},
    };
    p.project.index.apply_manifest(main, std::move(manifest));

    ZEXPECT(!p.chains(main, t).has_value());
    auto once = p.chains(main, u);
    ZASSERT(once);
    ZEXPECT(once->size() == 1u);
};

ZEST_CASE(RankingSeesContributor) {
    /// A unit indexed after the ranking was taken names the header in its
    /// entries: the ranking read the index's manifests, and takes it in.
    TreeProject p;
    llvm::StringRef b_text = "#include <config.h>\n";
    auto a = p.write("a.cpp", "#include \"config.h\"\n");
    auto b = p.write("b.cpp", b_text);
    auto header = p.write("config.h", "");
    p.project.dep_graph.set_includes(a, 0, {{header}});
    p.project.dep_graph.build_reverse_map();
    ZEXPECT(ranked_hosts(p.project, header) == llvm::SmallVector<Fid>{a});

    index::TUManifest manifest;
    manifest.built_at = 1;
    manifest.tu_version = VersionID{p.version(b, b_text)};
    manifest.contributions = {
        {VersionID{p.version(header, "")}, 1}
    };
    p.project.index.apply_manifest(b, std::move(manifest));
    ZEXPECT(ranked_hosts(p.project, header) == (llvm::SmallVector<Fid>{a, b}));
};

ZEST_CASE(TreeFollowsItsInputs) {
    /// A preprocess run's tree serves the command it ran under while what
    /// it read holds: a file it read changing, or another command, wants a
    /// run of its own.
    TreeProject p;
    llvm::StringRef main_text = "#include \"a.h\"\n";
    auto main = p.write("main.cpp", main_text);
    auto a = p.write("a.h", "#pragma once\n");
    auto other = p.write("other.h", "int x;\n");
    p.project.dep_graph.set_includes(main, 0, {{a}});
    p.project.dep_graph.build_reverse_map();

    auto key = p.tree(main).key;
    p.project.hosting.trees.commit(key,
                                   {.root = VersionID{p.version(main, main_text)},
                                    .nodes = {{.file = p.version(a, "#pragma once\n"), .line = 1}}},
                                   {*current_dep(p.project.file_table, other)});
    ZEXPECT(p.chains(main, a)->size() == 1u);
    ZEXPECT(!include_chains(p.project, {main, key.second + 1}, a).has_value());

    p.write("other.h", "int y;\n");
    p.project.file_table.disk.end_turn();
    ZEXPECT(!p.chains(main, a).has_value());
};

ZEST_CASE(FailedTreeHostsNothing) {
    /// A unit whose command cannot preprocess it hosts nothing, and the
    /// search goes on past it, until the unit changes.
    TreeProject p;
    auto a = p.write("a.cpp", "#include \"t.h\"\n");
    auto b = p.write("b.cpp", "#include \"t.h\"\n");
    auto t = p.write("t.h", "");
    p.project.dep_graph.set_includes(a, 0, {{t}});
    p.project.dep_graph.set_includes(b, 0, {{t}});
    p.project.dep_graph.build_reverse_map();

    commit_failed_tree(p.project, p.tree(a));
    ZEXPECT(p.chains(a, t)->empty());
    auto search = find_host(p.project, t, nullptr);
    ZASSERT(search.missing);
    ZEXPECT(search.missing->key.first == b);

    p.write("a.cpp", "#include \"t.h\"\nint a;\n");
    p.project.file_table.disk.end_turn();
    ZEXPECT(!p.chains(a, t).has_value());
};

ZEST_CASE(TrialFollowsItsInputs) {
    /// A trial's result holds while what its compile read holds, and the
    /// header's text: for an open header, whose buffer moves with every
    /// keystroke, its disk content.
    TreeProject p;
    auto header = p.write("t.h", "Widget w;\n");
    auto dep = p.write("dep.h", "struct Widget;\n");
    auto& files = p.project.file_table;
    TrialKey key{header, 7};
    ZEXPECT(!self_contained(p.project, key, false).has_value());

    feature::FileDiagnostic missing{
        .level = DiagnosticLevel::Error,
        .id = clang::diag::err_unknown_typename,
    };
    // The result changes what the listing folds: one taken before is stale.
    auto epoch = p.project.context_epoch;
    commit_trial(p.project,
                 key,
                 {missing},
                 current_dep(files, header)->version,
                 {*current_dep(files, dep)});
    ZEXPECT(p.project.context_epoch != epoch);
    ZEXPECT(self_contained(p.project, key, false) == false);
    ZEXPECT(self_contained(p.project, key, true) == false);
    ZEXPECT(!self_contained(p.project, {header, 8}, false).has_value());

    // A trial of a buffer the disk does not hold: the editor takes it, a
    // reader of the disk does not.
    TrialKey buffer{header, 9};
    commit_trial(p.project,
                 buffer,
                 {},
                 files.intern_version(header, 42),
                 {*current_dep(files, dep)});
    ZEXPECT(self_contained(p.project, buffer, true) == true);
    ZEXPECT(!self_contained(p.project, buffer, false).has_value());

    p.write("t.h", "Widget w2;\n");
    files.disk.end_turn();
    ZEXPECT(!self_contained(p.project, key, false).has_value());
    ZEXPECT(!self_contained(p.project, key, true).has_value());
    ZEXPECT(!self_contained(p.project, buffer, true).has_value());

    TrialKey other{header, 10};
    commit_trial(p.project,
                 other,
                 {},
                 current_dep(files, header)->version,
                 {*current_dep(files, dep)});
    p.write("dep.h", "struct Widget {};\n");
    files.disk.end_turn();
    ZEXPECT(!self_contained(p.project, other, true).has_value());
};

ZEST_CASE(FailedContextRemembered) {
    /// A chain the synthesis cannot follow is remembered as such while its
    /// files hold, and synthesized again once one changes.
    TreeProject p;
    auto main = p.write("main.cpp", "int x;\n");
    auto t = p.write("t.h", "int t;\n");
    Host host{
        .file = main,
        .chain = {main, t},
        .lines = {1}
    };
    auto command = p.tree(main).command;
    auto failed = synthesize(p.project, t, host, command);
    ZEXPECT(failed.result.synthesized == nullptr);
    ZEXPECT(ranges::contains(failed.trace, main, &DepState::path_id));

    p.write("main.cpp", "#include \"t.h\"\n");
    p.project.file_table.disk.end_turn();
    auto context = synthesize(p.project, t, host, command);
    ZEXPECT(context.result.synthesized != nullptr);
};

};  // ZEST_SUITE(Hosting)

}  // namespace

}  // namespace clice::testing
