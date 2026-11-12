module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :project.discovery;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

ZEST_SUITE(Discovery) {

ZEST_CASE(DiscoverEveryNearby) {
    /// Discovery lists the root's database and every direct
    /// subdirectory's in name order, and the ones above a file up to the
    /// root nearest first.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[]");
    tmp.touch("out/compile_commands.json", "[]");
    tmp.touch("build/compile_commands.json", "[]");
    tmp.touch("deep/proj/compile_commands.json", "[]");
    CanonicalPath root(Spelling::absolute(tmp.root));
    Spelling spelled(root);
    auto found = discovered_databases(root);
    ZASSERT(found.size() == 3u);
    ZEXPECT(found[0].str() == Spelling("compile_commands.json", spelled).str());
    ZEXPECT(found[1].str() == Spelling("build/compile_commands.json", spelled).str());
    ZEXPECT(found[2].str() == Spelling("out/compile_commands.json", spelled).str());

    auto above =
        databases_above(CanonicalPath(Spelling::absolute(tmp.path("deep/proj/src"))), root);
    ZASSERT(above.size() == 2u);
    ZEXPECT(CanonicalPath(above[0]) ==
            CanonicalPath(
                Spelling::absolute(path::join(tmp.root, "deep", "proj", "compile_commands.json"))));
    ZEXPECT(CanonicalPath(above[1]) ==
            CanonicalPath(Spelling::absolute(path::join(tmp.root, "compile_commands.json"))));
};

ZEST_CASE(NestedProjectsLeftOut) {
    /// A subdirectory with a configuration file of its own is its own
    /// project's: discovery, the walk above a file and the deeper walk
    /// leave it and everything below it out, and keep a plain one.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[]");
    tmp.touch("plain/compile_commands.json", "[]");
    tmp.touch("sub/clice.toml", "");
    tmp.touch("sub/compile_commands.json", "[]");
    tmp.touch("sub/deep/build/x/compile_commands.json", "[]");
    tmp.touch("hidden/.clice/config.toml", "");
    tmp.touch("hidden/compile_commands.json", "[]");
    CanonicalPath root(Spelling::absolute(tmp.root));
    Spelling spelled(root);
    auto found = discovered_databases(root);
    ZASSERT(found.size() == 2u);
    ZEXPECT(found[0].str() == Spelling("compile_commands.json", spelled).str());
    ZEXPECT(found[1].str() == Spelling("plain/compile_commands.json", spelled).str());

    auto above = databases_above(CanonicalPath(Spelling::absolute(tmp.path("sub/deep/src"))), root);
    ZASSERT(above.size() == 1u);
    ZEXPECT(CanonicalPath(above[0]) ==
            CanonicalPath(Spelling::absolute(path::join(tmp.root, "compile_commands.json"))));
    ZEXPECT(deeper_databases(root, CanonicalPath()).empty());

    ZEXPECT(nested_project(root, CanonicalPath(Spelling::absolute(tmp.path("sub/deep/a.cpp")))) ==
            CanonicalPath(Spelling::absolute(tmp.path("sub"))));
    ZEXPECT(
        nested_project(root, CanonicalPath(Spelling::absolute(tmp.path("plain/a.cpp")))).empty());
    // The root's own configuration does not count.
    tmp.touch("clice.toml", "");
    ZEXPECT(
        nested_project(root, CanonicalPath(Spelling::absolute(tmp.path("plain/a.cpp")))).empty());
};

ZEST_CASE(DeeperDatabasesNamed) {
    /// The walk names what discovery passes over, three directories deep
    /// at most.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[]");
    tmp.touch("build/compile_commands.json", "[]");
    tmp.touch("build/debug/compile_commands.json", "[]");
    tmp.touch("out/build/x64-debug/compile_commands.json", "[]");
    tmp.touch("a/b/c/d/compile_commands.json", "[]");
    CanonicalPath root(Spelling::absolute(tmp.root));
    Spelling spelled(root);
    auto deeper = deeper_databases(root, CanonicalPath());
    ZASSERT(deeper.size() == 2u);
    ZEXPECT(deeper[0].str() == Spelling("build/debug/compile_commands.json", spelled).str());
    ZEXPECT(deeper[1].str() ==
            Spelling("out/build/x64-debug/compile_commands.json", spelled).str());
};

#ifndef _WIN32
ZEST_CASE(DiscoverSymlinkedBuild) {
    /// A build directory symlinked elsewhere keeps its database, for
    /// discovery and the walk below it alike.
    TempDir tmp;
    tmp.touch("elsewhere/build/compile_commands.json", "[]");
    tmp.mkdir("ws/sub");
    ZASSERT(::symlink(tmp.path("elsewhere/build").c_str(), tmp.path("ws/build").c_str()) == 0);
    ZASSERT(::symlink(tmp.path("elsewhere/build").c_str(), tmp.path("ws/sub/build").c_str()) == 0);
    CanonicalPath root(Spelling::absolute(tmp.path("ws")));

    auto found = discovered_databases(root);
    ZASSERT(found.size() == 1u);
    ZEXPECT(found[0].str() == Spelling("build/compile_commands.json", Spelling(root)).str());

    auto deeper = deeper_databases(root, CanonicalPath());
    ZASSERT(deeper.size() == 1u);
    ZEXPECT(deeper[0].str() == Spelling("sub/build/compile_commands.json", Spelling(root)).str());
};
#endif

ZEST_CASE(FindProjectRoot) {
    /// A file outside every folder belongs to the nearest ancestor holding
    /// a clice.toml or a database, directly or in its build directory.
    TempDir tmp;
    tmp.touch("lib/clice.toml", "");
    tmp.touch("app/build/compile_commands.json", "[]");
    tmp.touch("tool/compile_commands.json", "[]");
    tmp.touch("tool/sub/clice.toml", "");
    tmp.touch("lib/out/compile_commands.json", "[]");
    tmp.touch("solo/db/compile_commands.json", "[]");
    auto above = [&](llvm::StringRef start) {
        return find_project_root(Spelling::absolute(tmp.path(start))).str();
    };
    ZEXPECT(above("lib/src/deep") == Spelling::absolute(tmp.path("lib")).str());
    ZEXPECT(above("app/src") == Spelling::absolute(tmp.path("app")).str());
    ZEXPECT(above("tool/src") == Spelling::absolute(tmp.path("tool")).str());
    ZEXPECT(above("tool/sub/src") == Spelling::absolute(tmp.path("tool/sub")).str());
    ZEXPECT(above("none/src").empty());
    // A build directory beside the sources belongs to their project.
    ZEXPECT(above("app/build/gen") == Spelling::absolute(tmp.path("app")).str());
    ZEXPECT(above("lib/out") == Spelling::absolute(tmp.path("lib")).str());
    ZEXPECT(above("solo/db/gen") == Spelling::absolute(tmp.path("solo/db")).str());
};

#ifndef _WIN32
ZEST_CASE(ProjectRootThroughSymlink) {
    /// A directory reached through a symlink belongs to the project of the
    /// directory it links to.
    TempDir tmp;
    tmp.touch("repo/clice.toml", "");
    tmp.touch("repo/src/main.cpp", "");
    ZASSERT(::symlink(tmp.path("repo/src").c_str(), tmp.path("alias").c_str()) == 0);
    auto root = find_project_root(Spelling::absolute(tmp.path("alias")));
    ZEXPECT(CanonicalPath(root).str() == CanonicalPath(Spelling::absolute(tmp.path("repo"))).str());
};
#endif

ZEST_CASE(DefinesProject) {
    /// A directory is a project of its own when it holds a configuration
    /// file or a database where startup discovery looks.
    TempDir tmp;
    tmp.touch("toml/clice.toml", "");
    tmp.touch("hidden/.clice/config.toml", "");
    tmp.touch("db/build/compile_commands.json", "[]");
    tmp.touch("plain/src/main.cpp", "");
    tmp.touch("deep/a/b/compile_commands.json", "[]");
    ZEXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("toml")))));
    ZEXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("hidden")))));
    ZEXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("db")))));
    ZEXPECT(!defines_project(CanonicalPath(Spelling::absolute(tmp.path("plain")))));
    ZEXPECT(!defines_project(CanonicalPath(Spelling::absolute(tmp.path("deep")))));
};

};  // ZEST_SUITE(Discovery)

}  // namespace

}  // namespace clice::testing
