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
#include "vfs/path.h"

namespace clice::testing {

namespace {

/// A view over one checked-in layout under tests/data/cdb: its clice.toml
/// loaded the way the server loads it, every declared source loaded.
struct Layout {
    std::string root;
    Config config;
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{config, cdb, files};

    explicit Layout(llvm::StringRef name) :
        root(path::join(data_dir(), "cdb", name)),
        config(Config::load_from_workspace(CanonicalPath(Spelling::absolute(root)))) {
        build.reset_active(fallback_configuration(config));
        for(auto source: build.declared_sources()) {
            cdb.load(source);
        }
    }

    /// The file's identity, like every path the build hands out.
    CanonicalPath path(llvm::StringRef relative) const {
        return CanonicalPath(Spelling::absolute(path::join(root, relative)));
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

ZEST_SUITE(Build) {
    ZEST_CASE(RuleBoundDatabaseWins) {
        /// The workspace database and a rule's database both list lib/x.cpp:
        /// the rule matching the file puts its database first, while src/a.cpp
        /// only the workspace database knows.
        Layout layout("rules_bound");
        ASSERT(layout.cdb.source_count() == 2U);

        auto x = layout.build.entries(layout.fid("lib/x.cpp"));
        ASSERT(x.size() == 2U);
        EXPECT(has_arg(layout.render("lib/x.cpp"), "LIB"));
        EXPECT(has_arg(layout.render("src/a.cpp"), "ROOT"));

        /// Edits accumulate from the rules matching the file, headers included.
        auto edits = layout.build.edits(layout.path("lib/y.hxx")).edits;
        ASSERT(edits.size() == 1U);
        EXPECT(edits[0].kind == CommandEdit::Kind::Append);
        EXPECT(edits[0].flags == (std::vector<std::string>{"-x", "c++-header"}));
        EXPECT(layout.build.edits(layout.path("lib/x.cpp")).empty());

        auto members = layout.build.members();
        EXPECT(members.size() == 2U);
        for(auto member: members) {
            EXPECT(layout.build.indexed(layout.files.resolve(member)));
        }
    };

    ZEST_CASE(DefaultCommandMembers) {
        /// No database anywhere: the rule's default command serves the files
        /// its patterns claim, enumerates the matching sources as members, and
        /// a nested rule keeps some of them out of the index.
        Layout layout("default_command_only");
        EXPECT(layout.build.declared_sources().empty());
        EXPECT(layout.build.declares_sources());

        auto commands = layout.build.commands(layout.fid("src/main.cpp"));
        ASSERT(commands.size() == 1U);
        EXPECT(commands.front().source == CommandSource::Default);
        EXPECT(!layout.build.commands(layout.fid("src/main.cpp")).empty());
        auto rendered = layout.render("src/main.cpp");
        EXPECT(has_arg(rendered, "DEFAULTED"));
        EXPECT(has_arg(rendered, layout.path("include")));

        /// A file no rule claims has no command; the builtin fallback serves it.
        EXPECT(!!layout.build.commands(layout.fid("tools/other.cpp")).empty());
        EXPECT(has_arg(layout.render("tools/other.cpp"), "clang++"));

        auto members = layout.build.members();
        ASSERT(members.size() == 2U);
        EXPECT(llvm::is_contained(members, layout.fid("src/main.cpp")));
        EXPECT(llvm::is_contained(members, layout.fid("src/skip/vendored.cpp")));
        EXPECT(!llvm::is_contained(members, layout.fid("include/lib.h")));
        EXPECT(!llvm::is_contained(members, layout.fid("tools/other.cpp")));
        EXPECT(layout.indexed("src/main.cpp"));
        EXPECT(!layout.indexed("src/skip/vendored.cpp"));
    };

    ZEST_CASE(UnitPredicate) {
        /// A source a default-command rule claims is a unit of its own; a header
        /// the same rule matches is not, and neither is a file no rule claims. A
        /// database entry makes any file a unit.
        Layout defaults("default_command_only");
        EXPECT(defaults.build.unit(defaults.fid("src/main.cpp")));
        EXPECT(!defaults.build.unit(defaults.fid("include/lib.h")));
        EXPECT(!defaults.build.unit(defaults.fid("tools/other.cpp")));

        Layout bound("rules_bound");
        EXPECT(bound.build.unit(bound.fid("lib/x.cpp")));
        EXPECT(!bound.build.unit(bound.fid("lib/y.hxx")));
    };

    ZEST_CASE(LintSet) {
        /// Files outside the workspace are never linted; inside it, every
        /// matching rule must keep `lint` on.
        Layout layout("lint_rules");
        EXPECT(layout.build.lintable(layout.path("src/main.cpp")));
        EXPECT(layout.build.lintable(layout.path("include/api.h")));
        EXPECT(!layout.build.lintable(layout.path("vendor/lib.cpp")));
        EXPECT(!layout.build.lintable(layout.path("vendor/deep/lib.h")));
        EXPECT(!layout.build.lintable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
        EXPECT(!layout.build.lintable(
            CanonicalPath(Spelling::absolute(layout.root + "-sibling/x.cpp"))));
    };

#ifndef _WIN32
    ZEST_CASE(LintSetSymlinkedRoot) {
        /// The workspace is opened through a symlink: the configuration resolves
        /// the root, and its rules anchor where every file's identity lies.
        TempDir tmp;
        tmp.touch("real/clice.toml", "[[rules]]\npatterns = [\"vendor/**\"]\nlint = false\n");
        tmp.touch("real/src/main.cpp", "int main() { return 0; }\n");
        tmp.touch("real/vendor/lib.cpp", "int lib() { return 0; }\n");
        ASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);

        Config config =
            Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.path("link"))));
        EXPECT(config.workspace_root.str() ==
               CanonicalPath(Spelling::absolute(tmp.path("real"))).str());
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active(fallback_configuration(config));
        EXPECT(build.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/src/main.cpp")))));
        EXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/vendor/lib.cpp")))));
        EXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("elsewhere/x.cpp")))));
    };

    ZEST_CASE(PatternThroughSymlink) {
        TempDir tmp;
        tmp.touch("clice.toml", R"([[rules]]
patterns = ["vendor/**"]
lint = false
)");
        tmp.touch("third_party/lib.cpp", "int lib() { return 0; }\n");
        ASSERT(::symlink(tmp.path("third_party").c_str(), tmp.path("vendor").c_str()) == 0);

        Config config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active(fallback_configuration(config));
        EXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("vendor/lib.cpp")))));
    };

    ZEST_CASE(WalkResolvesLinks) {
        /// A symlink under a pattern root names the file it points to, which
        /// the pattern does not claim.
        TempDir tmp;
        tmp.touch("src/main.cpp", "int main() {}\n");
        tmp.touch("elsewhere/impl.cpp", "");
        ASSERT(::symlink(tmp.path("elsewhere/impl.cpp").c_str(),
                         tmp.path("src/alias.cpp").c_str()) == 0);

        Config config;
        config.rules.push_back(
            ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto members = build.members();
        ASSERT(members.size() == 1U);
        EXPECT(files.resolve(members.front()) ==
               CanonicalPath(Spelling::absolute(tmp.path("src/main.cpp"))));
    };
#endif

    ZEST_CASE(FormatSet) {
        /// The format set reads its own rule field: a `lint = false` directory
        /// still formats, a `format = false` one does not.
        Layout layout("lint_rules");
        EXPECT(layout.build.formattable(layout.path("src/main.cpp")));
        EXPECT(layout.build.formattable(layout.path("vendor/lib.cpp")));
        EXPECT(!layout.build.formattable(layout.path("gen/out.h")));
        EXPECT(layout.build.lintable(layout.path("gen/out.h")));
        EXPECT(
            !layout.build.formattable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
    };

    ZEST_CASE(PatternRootsEnumerate) {
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
        config.rules.push_back(under_clice({.patterns = {"${workspace}/src/**"},
                                            .default_command = std::string("clang++ -DSRC")}));
        config.rules.push_back(under_clice(
            {.patterns = {"../lib/*.cpp"}, .default_command = std::string("clang++ -DLIB")}));
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        ASSERT(config.compiled_rules.size() == 2U);
        EXPECT(config.compiled_rules[0].patterns[0].root ==
               CanonicalPath(Spelling::absolute(tmp.path("src"))));
        EXPECT(config.compiled_rules[1].patterns[0].root ==
               CanonicalPath(Spelling::absolute(tmp.path("lib"))));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto members = build.members();
        ASSERT(members.size() == 2U);
        EXPECT(llvm::is_contained(members,
                                  files.intern(Spelling::absolute(tmp.path("src/main.cpp")))));
        EXPECT(llvm::is_contained(members,
                                  files.intern(Spelling::absolute(tmp.path("lib/util.cpp")))));
        EXPECT(
            build.commands(files.intern(Spelling::absolute(tmp.path("other/skip.cpp")))).empty());
        auto util = build.commands(files.intern(Spelling::absolute(tmp.path("lib/util.cpp"))));
        ASSERT(util.size() == 1U);
        EXPECT(has_arg(cdb.render_full(util.front().config), "LIB"));
    };

    ZEST_CASE(EditHashKeepsAnchor) {
        /// A rule's relative paths mean another directory once its
        /// configuration moves under .clice/, so its edit hash changes too.
        TempDir tmp;
        tmp.touch("main.cpp", "");
        auto hash = [&](llvm::StringRef directory) {
            Config config;
            config.rules.push_back(ConfigRule{.append = {"-Iinc"}});
            config.rules.back().directory = directory;
            config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
            FileTable files;
            CompilationDatabase cdb{files};
            Build build{config, cdb, files};
            CanonicalPath main(Spelling::absolute(tmp.path("main.cpp")));
            CanonicalRef file = main;
            return build.edit_hash(file);
        };
        EXPECT(hash(tmp.root) == hash(tmp.root));
        EXPECT(hash(tmp.root) != hash(tmp.path(".clice")));
    };

    ZEST_CASE(ForcedLanguageMembers) {
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
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto members = build.members();
        ASSERT(members.size() == 3U);
        EXPECT(llvm::is_contained(members, files.intern(Spelling::absolute(tmp.path("src/tool")))));
        EXPECT(
            llvm::is_contained(members, files.intern(Spelling::absolute(tmp.path("src/pre.i")))));
        EXPECT(llvm::is_contained(members,
                                  files.intern(Spelling::absolute(tmp.path("src/iface.cppm")))));
    };

    ZEST_CASE(WorkspaceRuleClaimsKnownSources) {
        /// A rule without patterns applies to every file, so its forced language
        /// claims only what clang recognizes as a source: the configuration file
        /// and an extensionless script stay out until a pattern names the script.
        TempDir tmp;
        tmp.touch("clice.toml", "");
        tmp.touch("main.cpp", "");
        tmp.touch("tool", "");
        Config config;
        config.rules.push_back(ConfigRule{.default_command = std::string("clang++ -x c++")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto members = build.members();
        ASSERT(members.size() == 1U);
        EXPECT(members.front() == files.intern(Spelling::absolute(tmp.path("main.cpp"))));

        config.rules.insert(
            config.rules.begin(),
            ConfigRule{.patterns = {"tool"}, .default_command = std::string("clang++ -x c++")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        build.reset_active("");
        members = build.members();
        EXPECT(members.size() == 2U);
        EXPECT(llvm::is_contained(members, files.intern(Spelling::absolute(tmp.path("tool")))));
    };

    ZEST_CASE(UnitsDeduplicated) {
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
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        for(auto source: build.declared_sources()) {
            cdb.load(source);
        }
        auto members = build.members();
        ASSERT(members.size() == 1U);
        EXPECT(build.entries(members.front()).size() == 3U);
        EXPECT(build.units(members).size() == 2U);
    };

    ZEST_CASE(CudaHeaderNotDefaultSource) {
        /// A `.cuh` under a rule forcing CUDA stays a header: clang's extension
        /// table does not know the suffix, but it names no unit.
        TempDir tmp;
        tmp.touch("cuda/kernel.cu", "");
        tmp.touch("cuda/kernel.cuh", "");
        Config config;
        config.rules.push_back(
            ConfigRule{.patterns = {"cuda/**"}, .default_command = std::string("clang++ -x cuda")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        EXPECT(build.members().size() == 1U);
        EXPECT(!build.unit(files.intern(Spelling::absolute(tmp.path("cuda/kernel.cuh")))));
    };

    ZEST_CASE(InvalidDefaultCommandIgnored) {
        /// A default command that names no compiler leaves its files without a
        /// command instead of aborting: they take the builtin fallback.
        TempDir tmp;
        tmp.touch("main.cpp", "");
        Config config;
        config.rules.push_back(ConfigRule{.default_command = std::string("ccache")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
        EXPECT(build.commands(main).empty());
        EXPECT(build.members().size() == 1U);
        EXPECT(build.builtin(CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))) !=
               invalid_config);
    };

    ZEST_CASE(UnmatchableRuleDeclaresNothing) {
        /// A default-command rule whose every pattern is invalid can claim no
        /// file, so it declares no source and discovery stays on; a database it
        /// names still counts, since entries apply regardless of patterns.
        TempDir tmp;
        Config config;
        config.rules.push_back(ConfigRule{
            .patterns = {"**/****.{c,cc}"},
            .default_command = std::string("clang++"),
        });
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        EXPECT(!build.declares_sources());
        EXPECT(build.members().empty());

        config.rules.push_back(ConfigRule{
            .patterns = {"**/****.{c,cc}"},
            .compile_commands = {"build"},
        });
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        build.reset_active("");
        EXPECT(build.declares_sources());
    };

    ZEST_CASE(DiscoveredSourceOrder) {
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
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto sub = cdb.add_source(Spelling::absolute(tmp.path("sub")));
        auto build_dir = cdb.add_source(Spelling::absolute(tmp.path("build")));
        auto root = cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
        for(auto id: {sub, build_dir, root}) {
            ASSERT(cdb.load_source(id));
            EXPECT(build.discovered(id));
        }

        auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
        EXPECT(build.source_order(files.resolve(main)) ==
               (llvm::SmallVector<SourceID, 4>{root, build_dir, sub}));
        EXPECT(build.entries(main).front().source == root);

        cdb.set_present(root, false);
        EXPECT(build.source_order(files.resolve(main)) ==
               (llvm::SmallVector<SourceID, 4>{build_dir, sub, root}));
        EXPECT(build.entries(main).front().source == build_dir);
    };

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
        auto found = discover_compile_commands(root);
        ASSERT(found.size() == 3u);
        EXPECT(found[0].str() == Spelling("compile_commands.json", spelled).str());
        EXPECT(found[1].str() == Spelling("build/compile_commands.json", spelled).str());
        EXPECT(found[2].str() == Spelling("out/compile_commands.json", spelled).str());

        auto above =
            compile_commands_above(CanonicalPath(Spelling::absolute(tmp.path("deep/proj/src"))),
                                   root);
        ASSERT(above.size() == 2u);
        EXPECT(CanonicalPath(above[0]) ==
               CanonicalPath(Spelling::absolute(
                   path::join(tmp.root, "deep", "proj", "compile_commands.json"))));
        EXPECT(CanonicalPath(above[1]) ==
               CanonicalPath(Spelling::absolute(path::join(tmp.root, "compile_commands.json"))));
    };

#ifndef _WIN32
    ZEST_CASE(DiscoverSymlinkedBuild) {
        /// A build directory symlinked elsewhere keeps its database, for the
        /// server's discovery and the batch commands' walk alike.
        TempDir tmp;
        tmp.touch("elsewhere/build/compile_commands.json", "[]");
        tmp.mkdir("ws");
        ASSERT(::symlink(tmp.path("elsewhere/build").c_str(), tmp.path("ws/build").c_str()) == 0);
        CanonicalPath root(Spelling::absolute(tmp.path("ws")));
        auto expected = Spelling("build/compile_commands.json", Spelling(root)).str();

        auto found = discover_compile_commands(root);
        ASSERT(found.size() == 1u);
        EXPECT(found[0].str() == expected);

        auto below = compile_commands_below(root, CanonicalPath());
        ASSERT(below.size() == 1u);
        EXPECT(below[0].str() == expected);
    };

    ZEST_CASE(LinkedBuildKeyedByLink) {
        /// A build directory symlinked elsewhere stays one database however it
        /// is retargeted: switching the link switches the commands.
        TempDir tmp;
        tmp.touch("out/debug/compile_commands.json", "[]");
        tmp.touch("out/release/compile_commands.json", "[]");
        ASSERT(::symlink(tmp.path("out/debug").c_str(), tmp.path("build").c_str()) == 0);
        FileTable files;
        CompilationDatabase cdb{files};
        auto build = Spelling::absolute(tmp.path("build"));
        auto first = cdb.add_source(build);

        ASSERT(::unlink(tmp.path("build").c_str()) == 0);
        ASSERT(::symlink(tmp.path("out/release").c_str(), tmp.path("build").c_str()) == 0);
        EXPECT(cdb.add_source(build) == first);
    };
#endif

    ZEST_CASE(ProjectRootAbove) {
        /// A file outside every folder belongs to the nearest ancestor holding
        /// a clice.toml or a database, directly or in its build directory.
        TempDir tmp;
        tmp.touch("lib/clice.toml", "");
        tmp.touch("app/build/compile_commands.json", "[]");
        tmp.touch("tool/compile_commands.json", "[]");
        tmp.touch("tool/sub/clice.toml", "");
        EXPECT(project_root_above(CanonicalPath(Spelling::absolute(tmp.path("lib/src/deep")))) ==
               CanonicalPath(Spelling::absolute(path::join(tmp.root, "lib"))));
        EXPECT(project_root_above(CanonicalPath(Spelling::absolute(tmp.path("app/src")))) ==
               CanonicalPath(Spelling::absolute(tmp.path("app"))));
        EXPECT(project_root_above(CanonicalPath(Spelling::absolute(tmp.path("tool/src")))) ==
               CanonicalPath(Spelling::absolute(tmp.path("tool"))));
        EXPECT(project_root_above(CanonicalPath(Spelling::absolute(tmp.path("tool/sub/src")))) ==
               CanonicalPath(Spelling::absolute(tmp.path("tool/sub"))));
        EXPECT(project_root_above(CanonicalPath(Spelling::absolute(tmp.path("none/src")))).empty());
    };

    ZEST_CASE(DefinesProject) {
        /// A directory is a project of its own when it holds a configuration
        /// file or a database where startup discovery looks.
        TempDir tmp;
        tmp.touch("toml/clice.toml", "");
        tmp.touch("hidden/.clice/config.toml", "");
        tmp.touch("db/build/compile_commands.json", "[]");
        tmp.touch("plain/src/main.cpp", "");
        tmp.touch("deep/a/b/compile_commands.json", "[]");
        EXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("toml")))));
        EXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("hidden")))));
        EXPECT(defines_project(CanonicalPath(Spelling::absolute(tmp.path("db")))));
        EXPECT(!defines_project(CanonicalPath(Spelling::absolute(tmp.path("plain")))));
        EXPECT(!defines_project(CanonicalPath(Spelling::absolute(tmp.path("deep")))));
    };

#ifndef _WIN32
    ZEST_CASE(ResolvedSpelling) {
        /// Two spellings of one directory resolve alike, created or not: the
        /// cache directories of two projects compare by it.
        TempDir tmp;
        tmp.touch("real/file", "");
        [[maybe_unused]] auto linked =
            ::symlink(tmp.path("real").c_str(), tmp.path("link").c_str());
        auto real = CanonicalPath(Spelling::absolute(tmp.path("real")));
        EXPECT(CanonicalPath(Spelling::absolute(tmp.path("link"))) == real);
        EXPECT(CanonicalPath(Spelling::absolute(tmp.path("link/.clice"))).str() ==
               path::join(real, ".clice"));
        EXPECT(CanonicalPath(Spelling::absolute(tmp.path("real/.clice"))).str() ==
               path::join(real, ".clice"));
    };
#endif

    ZEST_CASE(RefreshDefaultSources) {
        /// A file created under a default-command rule's patterns appears at
        /// the next refresh, once; a deleted one leaves the members.
        TempDir tmp;
        tmp.touch("src/main.cpp", "");
        Config config;
        config.rules.push_back(
            ConfigRule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");
        auto refresh = [&] {
            return build.refresh_default_sources(walk_sources(build.source_walk()));
        };
        ASSERT(build.members().size() == 1u);
        EXPECT(refresh().empty());

        tmp.touch("src/later.cpp", "");
        auto later = files.intern(Spelling::absolute(tmp.path("src/later.cpp")));
        EXPECT(refresh() == llvm::SmallVector<Fid>{later});
        EXPECT(refresh().empty());
        EXPECT(build.members().size() == 2u);

        vfs::remove_all(tmp.path("src/later.cpp"));
        EXPECT(refresh().empty());
        EXPECT(build.members().size() == 1u);
    };

    ZEST_CASE(DeclaredSourceOffDiscovery) {
        /// A rule declaring a default command is the whole intent: the database
        /// sitting at the root is not consulted.
        Layout layout("declared_ignores_discovered");
        EXPECT(layout.build.declares_sources());
        EXPECT(layout.build.declared_sources().empty());
        EXPECT(layout.cdb.source_count() == 0U);
        EXPECT(has_arg(layout.render("main.cpp"), "FROM_RULE"));
    };

    ZEST_CASE(InactiveConfigurationExcluded) {
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
        config.rules.push_back(
            ConfigRule{.configuration = "release", .compile_commands = {"release"}});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active(resolve_configuration(config, ""));
        EXPECT(build.active_configuration() == "release");
        ASSERT(build.declared_sources().size() == 1U);
        cdb.load(tmp.path("release"));
        cdb.load(tmp.path("debug"));
        ASSERT(cdb.source_count() == 2U);

        auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
        auto candidates = build.entries(main);
        ASSERT(candidates.size() == 1U);
        EXPECT(has_arg(cdb.render_full(candidates.front().config), "RELEASE"));
        EXPECT(build.edits(CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))).empty());
    };

    ZEST_CASE(InactiveSourceKeepsDiscovery) {
        /// Only an inactive configuration declares a database: the active one
        /// has no source, so discovery stays on for it.
        TempDir tmp;
        Config config;
        config.default_configuration = "release";
        config.rules.push_back(ConfigRule{.configuration = "debug", .compile_commands = {"."}});
        config.rules.push_back(ConfigRule{.configuration = "release", .append = {"-DNDEBUG"}});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active(resolve_configuration(config, ""));
        EXPECT(!build.declares_sources());
        EXPECT(build.declared_sources().empty());

        /// The database the inactive configuration names is the one discovery
        /// finds at the root: registered, it serves as a discovered source
        /// rather than hiding behind the inactive declaration.
        tmp.touch("main.cpp", "");
        write_cdb(tmp,
                  cdb,
                  build_cdb_json({
                      {tmp.root, tmp.path("main.cpp"), {}}
        }));
        EXPECT(build.entries(files.intern(Spelling::absolute(tmp.path("main.cpp")))).size() == 1U);
        EXPECT(build.members().size() == 1U);
    };

    ZEST_CASE(EditsAcrossHostAndHeader) {
        /// A header borrowing a host command carries both files' edits, each
        /// rule once, in declaration order.
        TempDir tmp;
        Config config;
        config.rules.push_back(ConfigRule{.patterns = {"src/**"}, .append = {"-DA"}});
        config.rules.push_back(ConfigRule{.patterns = {"include/**"}, .append = {"-DB"}});
        config.rules.push_back(
            ConfigRule{.patterns = {"**/*"}, .append = {"-DC"}, .remove = {"-DA"}});
        config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

        FileTable files;
        CompilationDatabase cdb{files};
        Build build{config, cdb, files};
        build.reset_active("");

        CanonicalPath host(Spelling::absolute(tmp.path("src/main.cpp")));
        CanonicalPath header(Spelling::absolute(tmp.path("include/x.h")));
        CanonicalRef both[] = {host, header};
        auto edits = build.edits(both).edits;
        ASSERT(edits.size() == 4U);
        EXPECT(edits[0].flags == (std::vector<std::string>{"-DA"}));
        EXPECT(edits[1].flags == (std::vector<std::string>{"-DB"}));
        EXPECT(edits[2].kind == CommandEdit::Kind::Remove);
        EXPECT(edits[2].flags == (std::vector<std::string>{"-DA"}));
        EXPECT(edits[3].flags == (std::vector<std::string>{"-DC"}));

        auto header_only = build.edits(header).edits;
        ASSERT(header_only.size() == 3U);
        EXPECT(header_only[0].flags == (std::vector<std::string>{"-DB"}));
    };

};  // ZEST_SUITE(Build)

}  // namespace

}  // namespace clice::testing
