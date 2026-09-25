#ifndef _WIN32
#include <unistd.h>
#endif

#include "test/cdb_helper.h"
#include "test/platform.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "project/build.h"
#include "project/configuration.h"
#include "project/project.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

/// The canonical spelling of a temp path: the build matches and hands out
/// canonical paths, TempDir spells them natively.
std::string canonical(const TempDir& tmp, llvm::StringRef relative) {
    auto p = tmp.path(relative);
    path::canonicalize(p);
    return p;
}

/// A view over one checked-in layout under tests/data/cdb: its clice.toml
/// loaded the way the server loads it, every declared source loaded.
struct Layout {
    std::string root;
    Config config;
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};

    explicit Layout(llvm::StringRef name) :
        root(path::join(data_dir(), "cdb", name)), config(Config::load_from_workspace(root)) {
        build.reset_active(fallback_configuration(config));
        for(auto source: build.declared_sources()) {
            cdb.load(source);
        }
    }

    /// The file's identity, like every path the build hands out.
    CanonicalPath path(llvm::StringRef relative) const {
        return CanonicalPath(path::join(root, relative));
    }

    Fid fid(llvm::StringRef relative) {
        return files.intern(path(relative));
    }

    /// The driver-level render of the file's default selection, or of the
    /// builtin fallback when the build does not compile it.
    std::vector<const char*> render(llvm::StringRef relative) {
        auto identity = path(relative);
        CanonicalRef file = identity;
        auto id = files.intern(file);
        auto commands = build.commands(id);
        auto ref =
            commands.empty()
                ? build.resolve(id, build.builtin(file), CommandSource::Fallback, file, file)
                : build.resolve(id, commands.front().config, commands.front().source, file, file);
        return cdb.render_driver(ref);
    }

    bool indexed(llvm::StringRef relative) {
        return build.indexed(path(relative));
    }
};

TEST_SUITE(Build) {

TEST_CASE(RuleBoundDatabaseWins) {
    /// The workspace database and a rule's database both list lib/x.cpp:
    /// the rule matching the file puts its database first, while src/a.cpp
    /// only the workspace database knows.
    Layout layout("rules_bound");
    ASSERT_EQ(layout.cdb.source_count(), 2U);

    auto x = layout.build.entries(layout.fid("lib/x.cpp"));
    ASSERT_EQ(x.size(), 2U);
    EXPECT_TRUE(has_arg(layout.render("lib/x.cpp"), "LIB"));
    EXPECT_TRUE(has_arg(layout.render("src/a.cpp"), "ROOT"));

    /// Edits accumulate from the rules matching the file, headers included.
    auto edits = layout.build.edits(layout.path("lib/y.hxx")).edits;
    ASSERT_EQ(edits.size(), 1U);
    EXPECT_EQ(edits[0].kind, CommandEdit::Kind::Append);
    EXPECT_EQ(edits[0].flags, (std::vector<std::string>{"-x", "c++-header"}));
    EXPECT_TRUE(layout.build.edits(layout.path("lib/x.cpp")).empty());

    auto members = layout.build.members();
    EXPECT_EQ(members.size(), 2U);
    for(auto member: members) {
        EXPECT_TRUE(layout.build.indexed(layout.files.resolve(member)));
    }
};

TEST_CASE(DefaultCommandMembers) {
    /// No database anywhere: the rule's default command serves the files
    /// its patterns claim, enumerates the matching sources as members, and
    /// a nested rule keeps some of them out of the index.
    Layout layout("default_command_only");
    EXPECT_TRUE(layout.build.declared_sources().empty());
    EXPECT_TRUE(layout.build.declares_sources());

    auto commands = layout.build.commands(layout.fid("src/main.cpp"));
    ASSERT_EQ(commands.size(), 1U);
    EXPECT_EQ(commands.front().source, CommandSource::Default);
    EXPECT_TRUE(!layout.build.commands(layout.fid("src/main.cpp")).empty());
    auto rendered = layout.render("src/main.cpp");
    EXPECT_TRUE(has_arg(rendered, "DEFAULTED"));
    EXPECT_TRUE(has_arg(rendered, layout.path("include")));

    /// A file no rule claims has no command; the builtin fallback serves it.
    EXPECT_FALSE(!layout.build.commands(layout.fid("tools/other.cpp")).empty());
    EXPECT_TRUE(has_arg(layout.render("tools/other.cpp"), "clang++"));

    auto members = layout.build.members();
    ASSERT_EQ(members.size(), 2U);
    EXPECT_TRUE(llvm::is_contained(members, layout.fid("src/main.cpp")));
    EXPECT_TRUE(llvm::is_contained(members, layout.fid("src/skip/vendored.cpp")));
    EXPECT_FALSE(llvm::is_contained(members, layout.fid("include/lib.h")));
    EXPECT_FALSE(llvm::is_contained(members, layout.fid("tools/other.cpp")));
    EXPECT_TRUE(layout.indexed("src/main.cpp"));
    EXPECT_FALSE(layout.indexed("src/skip/vendored.cpp"));
};

TEST_CASE(UnitPredicate) {
    /// A source a default-command rule claims is a unit of its own; a header
    /// the same rule matches is not, and neither is a file no rule claims. A
    /// database entry makes any file a unit.
    Layout defaults("default_command_only");
    EXPECT_TRUE(defaults.build.unit(defaults.fid("src/main.cpp")));
    EXPECT_FALSE(defaults.build.unit(defaults.fid("include/lib.h")));
    EXPECT_FALSE(defaults.build.unit(defaults.fid("tools/other.cpp")));

    Layout bound("rules_bound");
    EXPECT_TRUE(bound.build.unit(bound.fid("lib/x.cpp")));
    EXPECT_FALSE(bound.build.unit(bound.fid("lib/y.hxx")));
};

TEST_CASE(LintSet) {
    /// Files outside the workspace are never linted; inside it, every
    /// matching rule must keep `lint` on.
    Layout layout("lint_rules");
    EXPECT_TRUE(layout.build.lintable(layout.path("src/main.cpp")));
    EXPECT_TRUE(layout.build.lintable(layout.path("include/api.h")));
    EXPECT_FALSE(layout.build.lintable(layout.path("vendor/lib.cpp")));
    EXPECT_FALSE(layout.build.lintable(layout.path("vendor/deep/lib.h")));
    EXPECT_FALSE(layout.build.lintable(CanonicalPath("/usr/include/stdio.h")));
    EXPECT_FALSE(layout.build.lintable(CanonicalPath(layout.root + "-sibling/x.cpp")));
};

#ifndef _WIN32
TEST_CASE(LintSetSymlinkedRoot) {
    /// The workspace is opened through a symlink: the configuration resolves
    /// the root, and its rules anchor where every file's identity lies.
    TempDir tmp;
    tmp.touch("real/clice.toml", "[[rules]]\npatterns = [\"vendor/**\"]\nlint = false\n");
    tmp.touch("real/src/main.cpp", "int main() { return 0; }\n");
    tmp.touch("real/vendor/lib.cpp", "int lib() { return 0; }\n");
    ASSERT_EQ(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()), 0);

    Config config = Config::load_from_workspace(tmp.path("link"));
    EXPECT_EQ(config.workspace_root.str(), CanonicalPath(tmp.path("real")).str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active(fallback_configuration(config));
    EXPECT_TRUE(build.lintable(CanonicalPath(tmp.path("link/src/main.cpp"))));
    EXPECT_FALSE(build.lintable(CanonicalPath(tmp.path("link/vendor/lib.cpp"))));
    EXPECT_FALSE(build.lintable(CanonicalPath(tmp.path("elsewhere/x.cpp"))));
};

TEST_CASE(PatternThroughSymlink) {
    TempDir tmp;
    tmp.touch("clice.toml", "[[rules]]\npatterns = [\"vendor/**\"]\nlint = false\n");
    tmp.touch("third_party/lib.cpp", "int lib() { return 0; }\n");
    ASSERT_EQ(::symlink(tmp.path("third_party").c_str(), tmp.path("vendor").c_str()), 0);

    Config config = Config::load_from_workspace(tmp.root);
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active(fallback_configuration(config));
    EXPECT_FALSE(build.lintable(CanonicalPath(tmp.path("vendor/lib.cpp"))));
};
#endif

TEST_CASE(FormatSet) {
    /// The format set reads its own rule field: a `lint = false` directory
    /// still formats, a `format = false` one does not.
    Layout layout("lint_rules");
    EXPECT_TRUE(layout.build.formattable(layout.path("src/main.cpp")));
    EXPECT_TRUE(layout.build.formattable(layout.path("vendor/lib.cpp")));
    EXPECT_FALSE(layout.build.formattable(layout.path("gen/out.h")));
    EXPECT_TRUE(layout.build.lintable(layout.path("gen/out.h")));
    EXPECT_FALSE(layout.build.formattable(CanonicalPath("/usr/include/stdio.h")));
};

TEST_CASE(PatternRootsEnumerate) {
    /// Members are enumerated from where the patterns point, not from the
    /// configuration file's directory: a config under .clice/ claims
    /// sources through `${workspace}` and `..` patterns alike, and a
    /// pattern reaching outside the workspace is honoured too.
    TempDir tmp;
    tmp.touch("src/main.cpp", "int main() {}\n");
    tmp.touch("lib/util.cpp", "");
    tmp.touch("other/skip.cpp", "");

    Config config;
    auto under_clice = [&](ConfigRule rule) {
        rule.directory = tmp.path(".clice");
        return rule;
    };
    config.rules.push_back(under_clice(
        {.patterns = {"${workspace}/src/**"}, .default_command = std::string("clang++ -DSRC")}));
    config.rules.push_back(under_clice(
        {.patterns = {"../lib/*.cpp"}, .default_command = std::string("clang++ -DLIB")}));
    config.finalize(tmp.root.str());
    ASSERT_EQ(config.compiled_rules.size(), 2U);
    EXPECT_EQ(config.compiled_rules[0].patterns[0].root, CanonicalPath(canonical(tmp, "src")));
    EXPECT_EQ(config.compiled_rules[1].patterns[0].root, CanonicalPath(canonical(tmp, "lib")));

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ASSERT_EQ(members.size(), 2U);
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "src/main.cpp"))));
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "lib/util.cpp"))));
    EXPECT_TRUE(build.commands(files.intern(canonical(tmp, "other/skip.cpp"))).empty());
    auto util = build.commands(files.intern(canonical(tmp, "lib/util.cpp")));
    ASSERT_EQ(util.size(), 1U);
    EXPECT_TRUE(has_arg(cdb.render_full(util.front().config), "LIB"));
};

TEST_CASE(ForcedLanguageMembers) {
    /// An extensionless tool source joins the members when its default
    /// command forces the language; a header never does.
    TempDir tmp;
    tmp.touch("src/tool", "int main() {}\n");
    tmp.touch("src/util.h", "");
    tmp.touch("src/data.txt", "");
    tmp.touch("src/pre.i", "");
    tmp.touch("src/iface.cppm", "");
    Config config;
    config.rules.push_back(
        ConfigRule{.patterns = {"src/tool"}, .default_command = std::string("clang++ -x c++")});
    config.rules.push_back(ConfigRule{.patterns = {"src/**"},
                                      .default_command = std::string("clang++ -x c++-header")});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ASSERT_EQ(members.size(), 3U);
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "src/tool"))));
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "src/pre.i"))));
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "src/iface.cppm"))));
};

TEST_CASE(WorkspaceRuleClaimsKnownSources) {
    /// A rule without patterns applies to every file, so its forced language
    /// claims only what clang recognizes as a source: the configuration file
    /// and an extensionless script stay out until a pattern names the script.
    TempDir tmp;
    tmp.touch("clice.toml", "");
    tmp.touch("main.cpp", "");
    tmp.touch("tool", "");
    Config config;
    config.rules.push_back(ConfigRule{.default_command = std::string("clang++ -x c++")});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ASSERT_EQ(members.size(), 1U);
    EXPECT_EQ(members.front(), files.intern(canonical(tmp, "main.cpp")));

    config.rules.insert(
        config.rules.begin(),
        ConfigRule{.patterns = {"tool"}, .default_command = std::string("clang++ -x c++")});
    config.finalize(tmp.root.str());
    build.reset_active("");
    members = build.members();
    EXPECT_EQ(members.size(), 2U);
    EXPECT_TRUE(llvm::is_contained(members, files.intern(canonical(tmp, "tool"))));
};

TEST_CASE(UnitsDeduplicated) {
    /// Two databases listing a file with the same command yield one scan
    /// unit; a differing command stays its own unit.
    TempDir tmp;
    tmp.touch("main.cpp", "int main() {}\n");
    auto entry = [&](llvm::StringRef define) {
        return std::string(
                   R"([{"directory": "..", "file": "main.cpp", "arguments": ["clang++", ")") +
               define.str() + R"(", "main.cpp"]}])";
    };
    tmp.touch("a/compile_commands.json", entry("-DSAME"));
    tmp.touch("b/compile_commands.json", entry("-DSAME"));
    tmp.touch("c/compile_commands.json", entry("-DOTHER"));

    Config config;
    config.rules.push_back(ConfigRule{
        .compile_commands = {"a", "b", "c"}
    });
    config.finalize(tmp.root.str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    for(auto source: build.declared_sources()) {
        cdb.load(source);
    }
    auto members = build.members();
    ASSERT_EQ(members.size(), 1U);
    EXPECT_EQ(build.entries(members.front()).size(), 3U);
    EXPECT_EQ(build.units(members).size(), 2U);
};

TEST_CASE(CudaHeaderNotDefaultSource) {
    /// A `.cuh` under a rule forcing CUDA stays a header: clang's extension
    /// table does not know the suffix, but it names no unit.
    TempDir tmp;
    tmp.touch("cuda/kernel.cu", "");
    tmp.touch("cuda/kernel.cuh", "");
    Config config;
    config.rules.push_back(
        ConfigRule{.patterns = {"cuda/**"}, .default_command = std::string("clang++ -x cuda")});
    config.finalize(tmp.root.str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    EXPECT_EQ(build.members().size(), 1U);
    EXPECT_FALSE(build.unit(files.intern(canonical(tmp, "cuda/kernel.cuh"))));
};

TEST_CASE(InvalidDefaultCommandIgnored) {
    /// A default command that names no compiler leaves its files without a
    /// command instead of aborting: they take the builtin fallback.
    TempDir tmp;
    tmp.touch("main.cpp", "");
    Config config;
    config.rules.push_back(ConfigRule{.default_command = std::string("ccache")});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    auto main = files.intern(canonical(tmp, "main.cpp"));
    EXPECT_TRUE(build.commands(main).empty());
    EXPECT_EQ(build.members().size(), 1U);
    EXPECT_NE(build.builtin(canonical(tmp, "main.cpp")), invalid_config);
};

TEST_CASE(UnmatchableRuleDeclaresNothing) {
    /// A default-command rule whose every pattern is invalid can claim no
    /// file, so it declares no source and discovery stays on; a database it
    /// names still counts, since entries apply regardless of patterns.
    TempDir tmp;
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}"},
        .default_command = std::string("clang++"),
    });
    config.finalize(tmp.root.str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    EXPECT_FALSE(build.declares_sources());
    EXPECT_TRUE(build.members().empty());

    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}"},
        .compile_commands = {"build"},
    });
    config.finalize(tmp.root.str());
    build.reset_active("");
    EXPECT_TRUE(build.declares_sources());
};

TEST_CASE(DiscoveredSourceOrder) {
    /// Databases no rule declares rank by depth, then by path, the vanished
    /// after the present ones.
    TempDir tmp;
    auto listing = [&](llvm::StringRef file) {
        return build_cdb_json({
            {tmp.root, tmp.path(file), {}}
        });
    };
    tmp.touch("sub/compile_commands.json", listing("main.cpp"));
    tmp.touch("build/compile_commands.json", listing("main.cpp"));
    tmp.touch("compile_commands.json", listing("main.cpp"));
    Config config;
    config.finalize(tmp.root.str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    auto sub = cdb.add_source(tmp.path("sub"));
    auto build_dir = cdb.add_source(tmp.path("build"));
    auto root = cdb.add_source(tmp.path("compile_commands.json"));
    for(auto id: {sub, build_dir, root}) {
        ASSERT_TRUE(cdb.load_source(id).has_value());
        EXPECT_TRUE(build.discovered(id));
    }

    auto main = files.intern(canonical(tmp, "main.cpp"));
    EXPECT_EQ(build.source_order(files.resolve(main)),
              (llvm::SmallVector<SourceID, 4>{root, build_dir, sub}));
    EXPECT_EQ(build.entries(main).front().source, root);

    cdb.set_present(root, false);
    EXPECT_EQ(build.source_order(files.resolve(main)),
              (llvm::SmallVector<SourceID, 4>{build_dir, sub, root}));
    EXPECT_EQ(build.entries(main).front().source, build_dir);
};

TEST_CASE(DiscoverEveryNearby) {
    /// Discovery lists the root's database and every direct
    /// subdirectory's in name order, and the ones above a file up to the
    /// root nearest first.
    TempDir tmp;
    tmp.touch("compile_commands.json", "[]");
    tmp.touch("out/compile_commands.json", "[]");
    tmp.touch("build/compile_commands.json", "[]");
    tmp.touch("deep/proj/compile_commands.json", "[]");
    auto found = discover_compile_commands(tmp.root);
    ASSERT_EQ(found.size(), 3u);
    EXPECT_EQ(found[0], path::join(tmp.root, "compile_commands.json"));
    EXPECT_EQ(found[1], path::join(tmp.root, "build", "compile_commands.json"));
    EXPECT_EQ(found[2], path::join(tmp.root, "out", "compile_commands.json"));

    auto above = compile_commands_above(tmp.path("deep/proj/src"), tmp.root);
    ASSERT_EQ(above.size(), 2u);
    EXPECT_EQ(above[0], path::join(tmp.root, "deep", "proj", "compile_commands.json"));
    EXPECT_EQ(above[1], path::join(tmp.root, "compile_commands.json"));
};

TEST_CASE(ProjectRootAbove) {
    /// A file outside every folder belongs to the nearest ancestor holding
    /// a clice.toml or a database, directly or in its build directory.
    TempDir tmp;
    tmp.touch("lib/clice.toml", "");
    tmp.touch("app/build/compile_commands.json", "[]");
    tmp.touch("tool/compile_commands.json", "[]");
    tmp.touch("tool/sub/clice.toml", "");
    EXPECT_EQ(project_root_above(tmp.path("lib/src/deep")),
              CanonicalPath(path::join(tmp.root, "lib")));
    EXPECT_EQ(project_root_above(tmp.path("app/src")), CanonicalPath(tmp.path("app")));
    EXPECT_EQ(project_root_above(tmp.path("tool/src")), CanonicalPath(tmp.path("tool")));
    EXPECT_EQ(project_root_above(tmp.path("tool/sub/src")), CanonicalPath(tmp.path("tool/sub")));
    EXPECT_TRUE(project_root_above(tmp.path("none/src")).empty());
};

TEST_CASE(DefinesProject) {
    /// A directory is a project of its own when it holds a configuration
    /// file or a database where startup discovery looks.
    TempDir tmp;
    tmp.touch("toml/clice.toml", "");
    tmp.touch("hidden/.clice/config.toml", "");
    tmp.touch("db/build/compile_commands.json", "[]");
    tmp.touch("plain/src/main.cpp", "");
    tmp.touch("deep/a/b/compile_commands.json", "[]");
    EXPECT_TRUE(defines_project(tmp.path("toml")));
    EXPECT_TRUE(defines_project(tmp.path("hidden")));
    EXPECT_TRUE(defines_project(tmp.path("db")));
    EXPECT_FALSE(defines_project(tmp.path("plain")));
    EXPECT_FALSE(defines_project(tmp.path("deep")));
};

#ifndef _WIN32
TEST_CASE(ResolvedSpelling) {
    /// Two spellings of one directory resolve alike, created or not: the
    /// cache directories of two projects compare by it.
    TempDir tmp;
    tmp.touch("real/file", "");
    [[maybe_unused]] auto linked = ::symlink(tmp.path("real").c_str(), tmp.path("link").c_str());
    auto real = CanonicalPath(tmp.path("real"));
    EXPECT_EQ(CanonicalPath(tmp.path("link")), real);
    EXPECT_EQ(CanonicalPath(tmp.path("link/.clice")).str(), path::join(real, ".clice"));
    EXPECT_EQ(CanonicalPath(tmp.path("real/.clice")).str(), path::join(real, ".clice"));
};
#endif

TEST_CASE(RefreshDefaultSources) {
    /// A file created under a default-command rule's patterns appears at
    /// the next refresh, once; a deleted one leaves the members.
    TempDir tmp;
    tmp.touch("src/main.cpp", "");
    Config config;
    config.rules.push_back(
        ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    config.finalize(tmp.root.str());
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");
    ASSERT_EQ(build.members().size(), 1u);
    EXPECT_TRUE(build.refresh_default_sources().empty());

    tmp.touch("src/later.cpp", "");
    auto later = files.intern(canonical(tmp, "src/later.cpp"));
    EXPECT_EQ(build.refresh_default_sources(), llvm::SmallVector<Fid>{later});
    EXPECT_TRUE(build.refresh_default_sources().empty());
    EXPECT_EQ(build.members().size(), 2u);

    fs::remove_all(tmp.path("src/later.cpp"));
    EXPECT_TRUE(build.refresh_default_sources().empty());
    EXPECT_EQ(build.members().size(), 1u);
};

TEST_CASE(DeclaredSourceOffDiscovery) {
    /// A rule declaring a default command is the whole intent: the database
    /// sitting at the root is not consulted.
    Layout layout("declared_ignores_discovered");
    EXPECT_TRUE(layout.build.declares_sources());
    EXPECT_TRUE(layout.build.declared_sources().empty());
    EXPECT_EQ(layout.cdb.source_count(), 0U);
    EXPECT_TRUE(has_arg(layout.render("main.cpp"), "FROM_RULE"));
};

TEST_CASE(InactiveConfigurationExcluded) {
    /// Two tagged rules with their own databases: only the active tag's
    /// entries are candidates, even when the other database is loaded.
    TempDir tmp;
    tmp.touch("main.cpp", "int main() {}\n");
    auto entry = [&](llvm::StringRef define) {
        return std::string(
                   R"([{"directory": "..", "file": "main.cpp", "arguments": ["clang++", ")") +
               define.str() + R"(", "main.cpp"]}])";
    };
    tmp.touch("debug/compile_commands.json", entry("-DDEBUG"));
    tmp.touch("release/compile_commands.json", entry("-DRELEASE"));

    Config config;
    config.default_configuration = "release";
    config.rules.push_back(ConfigRule{.configuration = "debug",
                                      .compile_commands = {"debug"},
                                      .append = {"-DFROM_DEBUG_RULE"}});
    config.rules.push_back(ConfigRule{.configuration = "release", .compile_commands = {"release"}});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active(resolve_configuration(config, ""));
    EXPECT_EQ(build.active_configuration(), "release");
    ASSERT_EQ(build.declared_sources().size(), 1U);
    cdb.load(tmp.path("release"));
    cdb.load(tmp.path("debug"));
    ASSERT_EQ(cdb.source_count(), 2U);

    auto main = files.intern(canonical(tmp, "main.cpp"));
    auto candidates = build.entries(main);
    ASSERT_EQ(candidates.size(), 1U);
    EXPECT_TRUE(has_arg(cdb.render_full(candidates.front().config), "RELEASE"));
    EXPECT_TRUE(build.edits(CanonicalPath(canonical(tmp, "main.cpp"))).empty());
};

TEST_CASE(InactiveSourceKeepsDiscovery) {
    /// Only an inactive configuration declares a database: the active one
    /// has no source, so discovery stays on for it.
    TempDir tmp;
    Config config;
    config.default_configuration = "release";
    config.rules.push_back(ConfigRule{.configuration = "debug", .compile_commands = {"."}});
    config.rules.push_back(ConfigRule{.configuration = "release", .append = {"-DNDEBUG"}});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active(resolve_configuration(config, ""));
    EXPECT_FALSE(build.declares_sources());
    EXPECT_TRUE(build.declared_sources().empty());

    /// The database the inactive configuration names is the one discovery
    /// finds at the root: registered, it serves as a discovered source
    /// rather than hiding behind the inactive declaration.
    tmp.touch("main.cpp", "");
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    EXPECT_EQ(build.entries(files.intern(canonical(tmp, "main.cpp"))).size(), 1U);
    EXPECT_EQ(build.members().size(), 1U);
};

TEST_CASE(EditsAcrossHostAndHeader) {
    /// A header borrowing a host command carries both files' edits, each
    /// rule once, in declaration order.
    TempDir tmp;
    Config config;
    config.rules.push_back(ConfigRule{.patterns = {"src/**"}, .append = {"-DA"}});
    config.rules.push_back(ConfigRule{.patterns = {"include/**"}, .append = {"-DB"}});
    config.rules.push_back(ConfigRule{.patterns = {"**/*"}, .append = {"-DC"}, .remove = {"-DA"}});
    config.finalize(tmp.root.str());

    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};
    build.reset_active("");

    CanonicalPath host(canonical(tmp, "src/main.cpp"));
    CanonicalPath header(canonical(tmp, "include/x.h"));
    CanonicalRef both[] = {host, header};
    auto edits = build.edits(both).edits;
    ASSERT_EQ(edits.size(), 4U);
    EXPECT_EQ(edits[0].flags, (std::vector<std::string>{"-DA"}));
    EXPECT_EQ(edits[1].flags, (std::vector<std::string>{"-DB"}));
    EXPECT_EQ(edits[2].kind, CommandEdit::Kind::Remove);
    EXPECT_EQ(edits[2].flags, (std::vector<std::string>{"-DA"}));
    EXPECT_EQ(edits[3].flags, (std::vector<std::string>{"-DC"}));

    auto header_only = build.edits(header).edits;
    ASSERT_EQ(header_only.size(), 3U);
    EXPECT_EQ(header_only[0].flags, (std::vector<std::string>{"-DB"}));
};

};  // TEST_SUITE(Build)

}  // namespace

}  // namespace clice::testing
