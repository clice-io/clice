module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.database;
import :command.edit;
import :command.table;
import :project.build;
import :project.project;
import :settings.settings;
import :support.shell;
import :unittests.utils.cdb_helper;
import :unittests.utils.cdb_layouts;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

/// One of the layouts written to a temporary directory: its clice.toml
/// loaded the way the server loads it, every declared source loaded.
struct Layout {
    TempDir dir;
    std::string root;
    CanonicalPath project_root;
    Settings settings;
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};

    explicit Layout(llvm::StringRef name) :
        root(write_layout(dir, name)), project_root(Spelling::absolute(root)),
        settings(load_settings(project_root).settings) {
        build.reset_active(settings.fallback_configuration());
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
        auto candidates = build.commands(id);
        auto ref =
            build.apply_rules(id,
                              candidates.empty() ? Candidate{.base = build.builtin(file),
                                                             .source = CommandSource::Fallback}
                                                 : candidates.front());
        return commands.render_driver(ref, cdb.spelling(ref.file));
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
    ZASSERT(layout.cdb.source_count() == 2U);

    auto x = layout.build.entries(layout.fid("lib/x.cpp"));
    ZASSERT(x.size() == 2U);
    ZEXPECT(has_arg(layout.render("lib/x.cpp"), "LIB"));
    ZEXPECT(has_arg(layout.render("src/a.cpp"), "ROOT"));

    /// Edits accumulate from the rules matching the file, headers included.
    auto edits = layout.build.edits(layout.path("lib/y.hxx")).edits;
    ZASSERT(edits.size() == 1U);
    ZEXPECT(edits[0].kind == CommandEdit::Kind::Append);
    ZEXPECT(edits[0].flags == (std::vector<std::string>{"-x", "c++-header"}));
    ZEXPECT(layout.build.edits(layout.path("lib/x.cpp")).empty());

    auto members = layout.build.members();
    ZEXPECT(members.size() == 2U);
    for(auto member: members) {
        ZEXPECT(layout.build.indexed(layout.files.resolve(member)));
    }
};

ZEST_CASE(DefaultCommandMembers) {
    /// No database anywhere: the rule's default command serves the files
    /// its patterns cover, enumerates the matching sources as members, and
    /// a nested rule keeps some of them out of the index.
    Layout layout("default_command_only");
    ZEXPECT(layout.build.declared_sources().empty());
    ZEXPECT(layout.build.declares_sources());

    auto commands = layout.build.commands(layout.fid("src/main.cpp"));
    ZASSERT(commands.size() == 1U);
    ZEXPECT(commands.front().source == CommandSource::Default);
    ZEXPECT(!layout.build.commands(layout.fid("src/main.cpp")).empty());
    auto rendered = layout.render("src/main.cpp");
    ZEXPECT(has_arg(rendered, "DEFAULTED"));
    ZEXPECT(has_arg(rendered, layout.path("include")));

    /// A file no rule covers has no command; the builtin fallback serves it.
    ZEXPECT(!!layout.build.commands(layout.fid("tools/other.cpp")).empty());
    ZEXPECT(has_arg(layout.render("tools/other.cpp"), "clang++"));

    auto members = layout.build.members();
    ZASSERT(members.size() == 2U);
    ZEXPECT(ranges::contains(members, layout.fid("src/main.cpp")));
    ZEXPECT(ranges::contains(members, layout.fid("src/skip/vendored.cpp")));
    ZEXPECT(!ranges::contains(members, layout.fid("include/lib.h")));
    ZEXPECT(!ranges::contains(members, layout.fid("tools/other.cpp")));
    ZEXPECT(layout.indexed("src/main.cpp"));
    ZEXPECT(!layout.indexed("src/skip/vendored.cpp"));
};

ZEST_CASE(UnitPredicate) {
    /// A source a default-command rule covers is a unit of its own; a header
    /// the same rule matches is not, and neither is a file no rule covers. A
    /// database entry makes any file a unit.
    Layout defaults("default_command_only");
    ZEXPECT(defaults.build.unit(defaults.fid("src/main.cpp")));
    ZEXPECT(!defaults.build.unit(defaults.fid("include/lib.h")));
    ZEXPECT(!defaults.build.unit(defaults.fid("tools/other.cpp")));

    Layout bound("rules_bound");
    ZEXPECT(bound.build.unit(bound.fid("lib/x.cpp")));
    ZEXPECT(!bound.build.unit(bound.fid("lib/y.hxx")));
};

ZEST_CASE(LintSet) {
    /// Files outside the workspace are never linted; inside it, every
    /// matching rule must keep `lint` on.
    Layout layout("lint_rules");
    ZEXPECT(layout.build.lintable(layout.path("src/main.cpp")));
    ZEXPECT(layout.build.lintable(layout.path("include/api.h")));
    ZEXPECT(!layout.build.lintable(layout.path("vendor/lib.cpp")));
    ZEXPECT(!layout.build.lintable(layout.path("vendor/deep/lib.h")));
    ZEXPECT(!layout.build.lintable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
    ZEXPECT(
        !layout.build.lintable(CanonicalPath(Spelling::absolute(layout.root + "-sibling/x.cpp"))));
};

#ifndef _WIN32
ZEST_CASE(LintSetSymlinkedRoot) {
    /// The workspace is opened through a symlink: the settings resolve
    /// the root, and its rules anchor where every file's identity lies.
    TempDir tmp;
    tmp.touch("real/clice.toml", "[[rules]]\npatterns = [\"vendor/**\"]\nlint = false\n");
    tmp.touch("real/src/main.cpp", "int main() { return 0; }\n");
    tmp.touch("real/vendor/lib.cpp", "int lib() { return 0; }\n");
    ZASSERT(::symlink(tmp.path("real").c_str(), tmp.path("link").c_str()) == 0);

    CanonicalPath project_root(Spelling::absolute(tmp.path("link")));
    ZEXPECT(project_root.str() == CanonicalPath(Spelling::absolute(tmp.path("real"))).str());
    Settings settings = load_settings(project_root).settings;
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active(settings.fallback_configuration());
    ZEXPECT(build.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/src/main.cpp")))));
    ZEXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/vendor/lib.cpp")))));
    ZEXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("elsewhere/x.cpp")))));
};

ZEST_CASE(PatternThroughSymlink) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
patterns = ["vendor/**"]
lint = false
)");
    tmp.touch("third_party/lib.cpp", "int lib() { return 0; }\n");
    ZASSERT(::symlink(tmp.path("third_party").c_str(), tmp.path("vendor").c_str()) == 0);

    CanonicalPath project_root(Spelling::absolute(tmp.root));
    Settings settings = load_settings(project_root).settings;
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active(settings.fallback_configuration());
    ZEXPECT(!build.lintable(CanonicalPath(Spelling::absolute(tmp.path("vendor/lib.cpp")))));
};

ZEST_CASE(WalkResolvesLinks) {
    /// A symlink under a pattern root names the file it points to, which
    /// the pattern does not cover.
    TempDir tmp;
    tmp.touch("src/main.cpp", "int main() {}\n");
    tmp.touch("elsewhere/impl.cpp", "");
    ZASSERT(::symlink(tmp.path("elsewhere/impl.cpp").c_str(), tmp.path("src/alias.cpp").c_str()) ==
            0);

    Settings settings;
    settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ZASSERT(members.size() == 1U);
    ZEXPECT(files.resolve(members.front()) ==
            CanonicalPath(Spelling::absolute(tmp.path("src/main.cpp"))));
};
#endif

ZEST_CASE(DefaultSourcesLeaveNestedProjects) {
    /// A rule without patterns covers the whole tree but the projects
    /// nested in it.
    TempDir tmp;
    tmp.touch("clice.toml", "[[rules]]\ndefault_command = \"clang++ -std=c++20\"\n");
    tmp.touch("main.cpp", "int main() {}\n");
    tmp.touch("other/b.cpp", "");
    tmp.touch("sub/clice.toml", "");
    tmp.touch("sub/a.cpp", "");
    tmp.touch("tool/.clice/config.toml", "");
    tmp.touch("tool/c.cpp", "");

    CanonicalPath project_root(Spelling::absolute(tmp.root));
    Settings settings = load_settings(project_root).settings;
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active(settings.fallback_configuration());
    auto fid = [&](llvm::StringRef name) {
        return files.intern(CanonicalPath(Spelling::absolute(tmp.path(name))));
    };
    auto members = build.members();
    ZEXPECT(ranges::contains(members, fid("main.cpp")));
    ZEXPECT(ranges::contains(members, fid("other/b.cpp")));
    ZEXPECT(!ranges::contains(members, fid("sub/a.cpp")));
    ZEXPECT(!ranges::contains(members, fid("tool/c.cpp")));
};

ZEST_CASE(FormatSet) {
    /// The format set reads its own rule field: a `lint = false` directory
    /// still formats, a `format = false` one does not.
    Layout layout("lint_rules");
    ZEXPECT(layout.build.formattable(layout.path("src/main.cpp")));
    ZEXPECT(layout.build.formattable(layout.path("vendor/lib.cpp")));
    ZEXPECT(!layout.build.formattable(layout.path("gen/out.h")));
    ZEXPECT(layout.build.lintable(layout.path("gen/out.h")));
    ZEXPECT(!layout.build.formattable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
};

ZEST_CASE(PatternRootsEnumerate) {
    /// Members are enumerated from where the patterns point, not from the
    /// configuration file's directory: a configuration file under .clice/ covers
    /// sources through `${workspace}` and `..` patterns alike, and a
    /// pattern reaching outside the workspace is honoured too.
    TempDir tmp;
    tmp.touch("src/main.cpp", "int main() {}\n");
    tmp.touch("lib/util.cpp", "");
    tmp.touch("other/skip.cpp", "");

    Settings settings;
    auto under_clice = [&](Rule rule) {
        rule.directory = tmp.path(".clice");
        return rule;
    };
    settings.rules.push_back(under_clice(
        {.patterns = {"${workspace}/src/**"}, .default_command = std::string("clang++ -DSRC")}));
    settings.rules.push_back(under_clice(
        {.patterns = {"../lib/*.cpp"}, .default_command = std::string("clang++ -DLIB")}));
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    ZASSERT(settings.compiled_rules.size() == 2U);
    ZEXPECT(settings.compiled_rules[0].patterns[0].root ==
            CanonicalPath(Spelling::absolute(tmp.path("src"))));
    ZEXPECT(settings.compiled_rules[1].patterns[0].root ==
            CanonicalPath(Spelling::absolute(tmp.path("lib"))));

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ZASSERT(members.size() == 2U);
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/main.cpp")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("lib/util.cpp")))));
    ZEXPECT(build.commands(files.intern(Spelling::absolute(tmp.path("other/skip.cpp")))).empty());
    auto util = build.commands(files.intern(Spelling::absolute(tmp.path("lib/util.cpp"))));
    ZASSERT(util.size() == 1U);
    ZEXPECT(has_arg(commands.render_full(util.front().base), "LIB"));
};

ZEST_CASE(EditHashKeepsAnchor) {
    /// A rule's relative paths mean another directory once its
    /// configuration moves under .clice/, so its edit hash changes too.
    TempDir tmp;
    tmp.touch("main.cpp", "");
    auto hash = [&](llvm::StringRef directory) {
        Settings settings;
        settings.rules.push_back(Rule{.append = {"-Iinc"}});
        settings.rules.back().directory = directory;
        CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
        settings.finalize(project_root);
        FileTable files;
        CommandTable commands;
        CompilationDatabase cdb{files, commands};
        Build build{settings, project_root, cdb, files};
        return build.edit_hash(files.intern(Spelling::absolute(tmp.path("main.cpp"))), {});
    };
    ZEXPECT(hash(tmp.root) == hash(tmp.root));
    ZEXPECT(hash(tmp.root) != hash(tmp.path(".clice")));
};

ZEST_CASE(ForcedLanguageMembers) {
    /// A file clang has no language for joins the members when its default
    /// command forces the language and a pattern names it or its suffix; a
    /// header never does, nor a file only a wide pattern reaches.
    TempDir tmp;
    tmp.touch("src/tool", "int main() {}\n");
    tmp.touch("sketch/blink.ino", "");
    tmp.touch("sketch/README.md", "");
    tmp.touch("src/util.h", "");
    tmp.touch("src/data.txt", "");
    tmp.touch("src/pre.i", "");
    tmp.touch("src/iface.cppm", "");
    Settings settings;
    settings.rules.push_back(
        Rule{.patterns = {"src/tool"}, .default_command = std::string("clang++ -x c++")});
    settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++ -x c++-header")});
    settings.rules.push_back(Rule{
        .patterns = {"sketch/*.ino", "sketch/**"},
        .default_command = std::string("clang++ -x c++")
    });
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ZASSERT(members.size() == 4U);
    ZEXPECT(
        ranges::contains(members, files.intern(Spelling::absolute(tmp.path("sketch/blink.ino")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/tool")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/pre.i")))));
    ZEXPECT(
        ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/iface.cppm")))));
};

ZEST_CASE(WorkspaceRuleCoversKnownSources) {
    /// A rule without patterns applies to every file, so its forced language
    /// covers only what clang recognizes as a source: the configuration file
    /// and an extensionless script stay out until a pattern names the script.
    TempDir tmp;
    tmp.touch("clice.toml", "");
    tmp.touch("main.cpp", "");
    tmp.touch("tool", "");
    Settings settings;
    settings.rules.push_back(Rule{.default_command = std::string("clang++ -x c++")});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto members = build.members();
    ZASSERT(members.size() == 1U);
    ZEXPECT(members.front() == files.intern(Spelling::absolute(tmp.path("main.cpp"))));

    settings.rules.insert(
        settings.rules.begin(),
        Rule{.patterns = {"tool"}, .default_command = std::string("clang++ -x c++")});
    settings.finalize(project_root);
    build.reset_active("");
    members = build.members();
    ZEXPECT(members.size() == 2U);
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("tool")))));
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

    Settings settings;
    settings.rules.push_back(Rule{
        .compile_commands = {"a", "b", "c"}
    });
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    for(auto source: build.declared_sources()) {
        cdb.load(source);
    }
    auto members = build.members();
    ZASSERT(members.size() == 1U);
    ZEXPECT(build.entries(members.front()).size() == 3U);
    ZEXPECT(build.units(members).size() == 2U);
};

ZEST_CASE(CudaHeaderNotDefaultSource) {
    /// A `.cuh` under a rule forcing CUDA stays a header: clang's extension
    /// table does not know the suffix, but it names no unit.
    TempDir tmp;
    tmp.touch("cuda/kernel.cu", "");
    tmp.touch("cuda/kernel.cuh", "");
    Settings settings;
    settings.rules.push_back(
        Rule{.patterns = {"cuda/**"}, .default_command = std::string("clang++ -x cuda")});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    ZEXPECT(build.members().size() == 1U);
    ZEXPECT(!build.unit(files.intern(Spelling::absolute(tmp.path("cuda/kernel.cuh")))));
};

ZEST_CASE(InvalidDefaultCommandIgnored) {
    /// A default command that names no compiler leaves its files without a
    /// command instead of aborting: they take the builtin fallback.
    TempDir tmp;
    tmp.touch("main.cpp", "");
    Settings settings;
    settings.rules.push_back(Rule{.default_command = std::string("ccache")});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    ZEXPECT(build.commands(main).empty());
    ZEXPECT(build.members().size() == 1U);
    ZEXPECT(build.builtin(CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))) !=
            invalid_command);
};

ZEST_CASE(UnmatchableRuleDeclaresNothing) {
    /// A default-command rule whose every pattern is invalid can cover no
    /// file, so it declares no source and discovery stays on; a database it
    /// names still counts, since entries apply regardless of patterns.
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}"},
        .default_command = std::string("clang++"),
    });
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    ZEXPECT(!build.declares_sources());
    ZEXPECT(build.members().empty());

    settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}"},
        .compile_commands = {"build"},
    });
    settings.finalize(project_root);
    build.reset_active("");
    ZEXPECT(build.declares_sources());
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
    Settings settings;
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto sub = cdb.add_source(Spelling::absolute(tmp.path("sub")));
    auto build_dir = cdb.add_source(Spelling::absolute(tmp.path("build")));
    auto root = cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    for(auto id: {sub, build_dir, root}) {
        ZASSERT(cdb.load_source(id));
        ZEXPECT(build.discovered(id));
    }

    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    ZEXPECT(build.source_order(files.resolve(main)) ==
            (llvm::SmallVector<SourceID, 4>{root, build_dir, sub}));
    ZEXPECT(build.entries(main).front().source == root);

    cdb.set_present(root, false);
    ZEXPECT(build.source_order(files.resolve(main)) ==
            (llvm::SmallVector<SourceID, 4>{build_dir, sub, root}));
    ZEXPECT(build.entries(main).front().source == build_dir);
};

#ifndef _WIN32
ZEST_CASE(LinkedBuildKeyedByLink) {
    /// A build directory symlinked elsewhere stays one database however it
    /// is retargeted: switching the link switches the commands.
    TempDir tmp;
    tmp.touch("out/debug/compile_commands.json", "[]");
    tmp.touch("out/release/compile_commands.json", "[]");
    ZASSERT(::symlink(tmp.path("out/debug").c_str(), tmp.path("build").c_str()) == 0);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    auto build = Spelling::absolute(tmp.path("build"));
    auto first = cdb.add_source(build);

    ZASSERT(::unlink(tmp.path("build").c_str()) == 0);
    ZASSERT(::symlink(tmp.path("out/release").c_str(), tmp.path("build").c_str()) == 0);
    ZEXPECT(cdb.add_source(build) == first);
};

ZEST_CASE(LinkedDatabaseSameFile) {
    /// The root's link to the build's database is that database while the
    /// link stands.
    TempDir tmp;
    tmp.touch("build/compile_commands.json", "[]");
    ZASSERT(::symlink(tmp.path("build/compile_commands.json").c_str(),
                      tmp.path("compile_commands.json").c_str()) == 0);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    auto link = cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    auto target = Spelling::absolute(tmp.path("build/compile_commands.json"));
    ZEXPECT(cdb.find_same_file(target) == link);
    ZEXPECT(cdb.find_same_file(Spelling::absolute(tmp.path("compile_commands.json"))) ==
            std::nullopt);

    ZASSERT(::unlink(tmp.path("compile_commands.json").c_str()) == 0);
    ZEXPECT(cdb.find_same_file(target) == std::nullopt);
};
#endif

#ifndef _WIN32
ZEST_CASE(ResolvedSpelling) {
    /// Two spellings of one directory resolve alike, created or not: the
    /// cache directories of two projects compare by it.
    TempDir tmp;
    tmp.touch("real/file", "");
    [[maybe_unused]] auto linked = ::symlink(tmp.path("real").c_str(), tmp.path("link").c_str());
    auto real = CanonicalPath(Spelling::absolute(tmp.path("real")));
    ZEXPECT(CanonicalPath(Spelling::absolute(tmp.path("link"))) == real);
    ZEXPECT(CanonicalPath(Spelling::absolute(tmp.path("link/.clice"))).str() ==
            path::join(real, ".clice"));
    ZEXPECT(CanonicalPath(Spelling::absolute(tmp.path("real/.clice"))).str() ==
            path::join(real, ".clice"));
};
#endif

ZEST_CASE(RefreshDefaultSources) {
    /// A file created under a default-command rule's patterns appears at
    /// the next refresh, once; a deleted one leaves the members.
    TempDir tmp;
    tmp.touch("src/main.cpp", "");
    Settings settings;
    settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto refresh = [&] {
        return build.refresh_default_sources(walk_sources(build.source_walk()));
    };
    ZASSERT(build.members().size() == 1u);
    ZEXPECT(refresh().empty());

    tmp.touch("src/later.cpp", "");
    auto later = files.intern(Spelling::absolute(tmp.path("src/later.cpp")));
    ZEXPECT(refresh() == llvm::SmallVector<Fid>{later});
    ZEXPECT(refresh().empty());
    ZEXPECT(build.members().size() == 2u);

    fs::remove_all(tmp.path("src/later.cpp"));
    ZEXPECT(refresh().empty());
    ZEXPECT(build.members().size() == 1u);
};

ZEST_CASE(DeclaredSourceOffDiscovery) {
    /// A rule declaring a default command is the whole intent: the database
    /// sitting at the root is not consulted.
    Layout layout("declared_ignores_discovered");
    ZEXPECT(layout.build.declares_sources());
    ZEXPECT(layout.build.declared_sources().empty());
    ZEXPECT(layout.cdb.source_count() == 0U);
    ZEXPECT(has_arg(layout.render("main.cpp"), "FROM_RULE"));
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

    Settings settings;
    settings.default_configuration = "release";
    settings.rules.push_back(Rule{.configuration = "debug",
                                  .compile_commands = {"debug"},
                                  .append = {"-DFROM_DEBUG_RULE"}});
    settings.rules.push_back(Rule{.configuration = "release", .compile_commands = {"release"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active(settings.fallback_configuration());
    ZEXPECT(build.active_configuration() == "release");
    ZASSERT(build.declared_sources().size() == 1U);
    cdb.load(tmp.path("release"));
    cdb.load(tmp.path("debug"));
    ZASSERT(cdb.source_count() == 2U);

    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto candidates = build.entries(main);
    ZASSERT(candidates.size() == 1U);
    ZEXPECT(has_arg(commands.render_full(candidates.front().command), "RELEASE"));
    ZEXPECT(build.edits(CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))).empty());
};

ZEST_CASE(InactiveSourceKeepsDiscovery) {
    /// Only an inactive configuration declares a database: the active one
    /// has no source, so discovery stays on for it.
    TempDir tmp;
    Settings settings;
    settings.default_configuration = "release";
    settings.rules.push_back(Rule{.configuration = "debug", .compile_commands = {"."}});
    settings.rules.push_back(Rule{.configuration = "release", .append = {"-DNDEBUG"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active(settings.fallback_configuration());
    ZEXPECT(!build.declares_sources());
    ZEXPECT(build.declared_sources().empty());

    /// The database the inactive configuration names is the one discovery
    /// finds at the root: registered, it serves as a discovered source
    /// rather than hiding behind the inactive declaration.
    tmp.touch("main.cpp", "");
    write_cdb(tmp,
              cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZEXPECT(build.entries(files.intern(Spelling::absolute(tmp.path("main.cpp")))).size() == 1U);
    ZEXPECT(build.members().size() == 1U);
};

ZEST_CASE(EditsAcrossHostAndHeader) {
    /// A header borrowing a host command carries both files' edits, each
    /// rule once, in declaration order.
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DA"}});
    settings.rules.push_back(Rule{.patterns = {"include/**"}, .append = {"-DB"}});
    settings.rules.push_back(Rule{.patterns = {"**/*"}, .append = {"-DC"}, .remove = {"-DA"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");

    CanonicalPath host(Spelling::absolute(tmp.path("src/main.cpp")));
    CanonicalPath header(Spelling::absolute(tmp.path("include/x.h")));
    CanonicalRef both[] = {host, header};
    auto edits = build.edits(both).edits;
    ZASSERT(edits.size() == 4U);
    ZEXPECT(edits[0].flags == (std::vector<std::string>{"-DA"}));
    ZEXPECT(edits[1].flags == (std::vector<std::string>{"-DB"}));
    ZEXPECT(edits[2].kind == CommandEdit::Kind::Remove);
    ZEXPECT(edits[2].flags == (std::vector<std::string>{"-DA"}));
    ZEXPECT(edits[3].flags == (std::vector<std::string>{"-DC"}));

    auto header_only = build.edits(header).edits;
    ZASSERT(header_only.size() == 3U);
    ZEXPECT(header_only[0].flags == (std::vector<std::string>{"-DB"}));
};

ZEST_CASE(RuleTurnsQueryOff) {
    /// A matching rule turning the toolchain query off turns it off for the
    /// file's command, alone or beside other edits, and the edit hash says
    /// so; a command lent from such a file keeps it off.
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"src/**", "vendor/**"},
        .append = {"-DA"}
    });
    settings.rules.push_back(Rule{
        .patterns = {"vendor/**", "third_party/**"},
        .query_toolchain = false
    });
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");

    CanonicalPath own(Spelling::absolute(tmp.path("src/a.cpp")));
    CanonicalPath vendor(Spelling::absolute(tmp.path("vendor/b.cpp")));
    CanonicalPath third_party(Spelling::absolute(tmp.path("third_party/c.cpp")));
    CanonicalRef own_file = own;
    CanonicalRef vendor_file = vendor;
    CanonicalRef third_party_file = third_party;
    ZEXPECT(build.edits(own_file).query_toolchain);
    ZEXPECT(!build.edits(vendor_file).query_toolchain);
    ZEXPECT(build.edit_hash(files.intern(own_file), {}) !=
            build.edit_hash(files.intern(vendor_file), {}));
    ZEXPECT(build.edit_hash(files.intern(third_party_file), {}) != 0);

    // One command under the same appends: only the query tells them apart.
    auto command = std::format("clang++ {}", vendor.str());
    auto entry = *cdb.add_command(tmp.root, vendor.str(), llvm::StringRef(command));
    auto unqueried =
        build.apply_rules(entry.file, {.base = entry.command, .source = CommandSource::Database});
    auto queried = build.apply_rules(files.intern(own_file),
                                     {.base = entry.command, .source = CommandSource::Database});
    ZEXPECT(!commands.get(unqueried.command).query_toolchain);
    ZEXPECT(commands.get(queried.command).query_toolchain);

    auto borrowed = build.apply_rules(
        files.intern(own_file),
        {.base = entry.command, .source = CommandSource::Borrowed, .from = entry.file});
    ZEXPECT(!commands.get(borrowed.command).query_toolchain);
};

ZEST_CASE(BorrowMergesRules) {
    /// A borrowed command is edited by the rules matching the file it is
    /// from and the borrower, in declaration order, a rule matching both
    /// once.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"new/**"}, .append = {"-DOWN"}});
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DLENDER"}});
    settings.rules.push_back(Rule{.patterns = {"**/*"}, .append = {"-DBOTH"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto command = std::format("clang++ {}", tmp.path("src/a.cpp"));
    auto lender = *cdb.add_command(tmp.root, tmp.path("src/a.cpp"), llvm::StringRef(command));

    auto ref = build.apply_rules(
        files.intern(Spelling::absolute(tmp.path("new/n.cpp"))),
        {.base = lender.command, .source = CommandSource::Borrowed, .from = lender.file});
    auto argv = commands.render_driver(ref, cdb.spelling(ref.file));
    auto at = [&](llvm::StringRef macro) {
        return ranges::find_if(
                   argv,
                   [&](const char* arg) { return llvm::StringRef(arg).contains(macro); }) -
               argv.begin();
    };
    auto count = [&](llvm::StringRef macro) {
        return ranges::count_if(argv, [&](const char* arg) {
            return llvm::StringRef(arg).contains(macro);
        });
    };
    ZEXPECT(count("LENDER") == 1);
    ZEXPECT(count("OWN") == 1);
    ZEXPECT(count("BOTH") == 1);
    ZEXPECT(at("OWN") < at("LENDER"));
    ZEXPECT(at("LENDER") < at("BOTH"));
};

ZEST_CASE(DeclaredPCHTakesBorrowedRules) {
    /// The precompiled header a borrowed command reads builds under the
    /// rules that edited the command: the lender's as well as the
    /// borrower's.
    TempDir tmp;
    tmp.touch("pch.hxx", "");
    tmp.touch("pch.hxx.cxx", "");
    tmp.touch("src/a.cpp", "");
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DLENDER"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);

    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    Build build{settings, project_root, cdb, files};
    build.reset_active("");
    auto pch = tmp.path("pch.hxx.pch");
    auto writes = std::format("clang++ -Xclang -emit-pch -x c++-header -o {} {}",
                              pch,
                              tmp.path("pch.hxx.cxx"));
    cdb.add_command(tmp.root, tmp.path("pch.hxx.cxx"), llvm::StringRef(writes));
    auto reads =
        std::format("clang++ -Xclang -include-pch -Xclang {} {}", pch, tmp.path("src/a.cpp"));
    auto lender = *cdb.add_command(tmp.root, tmp.path("src/a.cpp"), llvm::StringRef(reads));

    auto borrower = files.intern(Spelling::absolute(tmp.path("new/n.cpp")));
    Candidate borrowed{.base = lender.command,
                       .source = CommandSource::Borrowed,
                       .from = lender.file};
    auto ref = build.apply_rules(borrower, borrowed);
    auto writer = build.pch_unit(borrower, borrowed, ref.command);
    ZASSERT(writer);
    ZEXPECT(has_arg(commands.render_full(writer->command), "LENDER"));
};

/// The builtin fallback render for a file without an entry and no default
/// command, resource dir stripped like render_entry.
std::vector<const char*> render_fallback(CompilationDatabase& db,
                                         llvm::StringRef file,
                                         const CommandEdits& options = {}) {
    Settings settings;
    CanonicalPath project_root = CanonicalPath();
    settings.finalize(project_root);
    Build build{settings, project_root, db, db.files()};
    auto source = CommandSource::Fallback;
    auto applied = apply_edits(db.commands(),
                               db.files(),
                               build.builtin(CanonicalPath(Spelling::absolute(file))),
                               options);
    CommandRef ref{db.files().intern(Spelling::absolute(file)),
                   applied,
                   db.commands().input_kind(applied, file),
                   source};
    auto argv = db.commands().render_driver(ref, db.spelling(ref.file));
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(llvm::StringRef(argv[i]) == "-resource-dir") {
            argv.erase(argv.begin() + i, argv.begin() + i + 2);
            break;
        }
    }
    return argv;
}

ZEST_CASE(DefaultFallback) {
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};

    /// C++ files get "clang++ -std=c++20 <file>".
    auto cpp_argv = render_fallback(database, fake("unknown.cpp"));
    ZASSERT(cpp_argv.size() == 3U);
    ZEXPECT(cpp_argv[0] == "clang++"sv);
    ZEXPECT(cpp_argv[1] == "-std=c++20"sv);
    ZEXPECT(cpp_argv[2] == fake("unknown.cpp"));

    /// Every extension clang classifies as C++ gets the C++ default.
    for(llvm::StringRef cxx_file: {"header.hpp", "file.cc", "file.cxx", "file.C", "file.hh"}) {
        ZEXPECT(render_fallback(database, fake(cxx_file))[0] == "clang++"sv);
    }

    /// C files get "clang <file>".
    auto c_argv = render_fallback(database, fake("unknown.c"));
    ZASSERT(c_argv.size() == 2U);
    ZEXPECT(c_argv[0] == "clang"sv);
    ZEXPECT(c_argv[1] == fake("unknown.c"));

    /// An ambiguous header is C++ by default, forced through -x so it
    /// compiles as a translation unit; other extensions get plain clang.
    auto h_argv = render_fallback(database, fake("foo.h"));
    ZASSERT(h_argv.size() == 5U);
    ZEXPECT(h_argv[0] == "clang++"sv);
    ZEXPECT(h_argv[2] == "-x"sv);
    ZEXPECT(h_argv[3] == "c++"sv);
    ZEXPECT(render_fallback(database, fake("foo.m"))[0] == "clang"sv);

    /// CUDA files pin cuda mode and the device-side view NVCC-backed
    /// commands default to.
    for(llvm::StringRef cuda_file: {"kern.cu", "kernels.cuh"}) {
        auto cu_argv = render_fallback(database, fake(cuda_file));
        ZASSERT(cu_argv.size() == 6U);
        ZEXPECT(cu_argv[0] == "clang++"sv);
        ZEXPECT(cu_argv[2] == "-x"sv);
        ZEXPECT(cu_argv[3] == "cuda"sv);
        ZEXPECT(cu_argv[4] == "--cuda-device-only"sv);
    }
};

ZEST_CASE(FallbackAppliesAppend) {
    /// Rule appends must reach the synthesized fallback command:
    /// users without a CDB rely on them to supply include paths.
    FileTable file_table;
    CommandTable commands;
    CompilationDatabase database{file_table, commands};
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-I/opt/include"}}
    };
    CommandEdits options{.edits = edits};

    auto argv = print_argv(render_fallback(database, fake("unknown.cpp"), options));
    ZEXPECT(llvm::StringRef(argv).contains("-std=c++20"));
    ZEXPECT(llvm::StringRef(argv).contains("/opt/include"));

    /// The plain-clang branch applies them too.
    ZEXPECT(llvm::StringRef(print_argv(render_fallback(database, fake("unknown.c"), options)))
                .contains("/opt/include"));
};

};  // ZEST_SUITE(Build)

}  // namespace

}  // namespace clice::testing
