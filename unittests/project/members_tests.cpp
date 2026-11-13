module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.argument;
import :command.database;
import :command.edit;
import :command.table;
import :index.project_index.manifest;
import :project.background;
import :project.command;
import :project.members;
import :project.project;
import :settings.rules;
import :settings.settings;
import :unittests.utils.cdb_helper;
import :unittests.utils.cdb_layouts;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.file_system;
import :vfs.path;

namespace clice::testing {

namespace {

using namespace std::literals;

/// Finalize the project's settings at its root, as loading them does, and
/// take their rules under the configuration they fall back to, as opening
/// the project does.
void activate(Project& project) {
    project.settings.finalize(project.root);
    project.rules =
        Rules(project.settings, project.root, project.settings.fallback_configuration());
}

/// One of the layouts written to a temporary directory, as a project: its
/// clice.toml loaded the way the server loads it, its rules under the
/// configuration the server activates, every declared database loaded.
struct Layout {
    TempDir dir;
    std::string root;
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(root))};

    explicit Layout(llvm::StringRef name) : root(write_layout(dir, name)) {
        project.settings = load_settings(project.root).settings;
        project.rules =
            Rules(project.settings, project.root, project.settings.fallback_configuration());
        for(auto source: project.rules.declared_databases()) {
            project.cdb.load(source);
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
        auto candidates = project.candidates(id);
        auto ref = project.apply_rules(id,
                                       candidates.empty()
                                           ? Candidate{.base = project.fallback_command(file),
                                                       .source = CommandSource::Fallback}
                                           : candidates.front());
        return project.commands.render_driver(ref, project.cdb.spelling(ref.file));
    }

    bool indexed(llvm::StringRef relative) {
        return project.rules.indexable(path(relative));
    }
};

ZEST_SUITE(Members) {

ZEST_CASE(RuleBoundDatabaseWins) {
    /// The workspace database and a rule's database both list lib/x.cpp:
    /// the rule matching the file puts its database first, while src/a.cpp
    /// only the workspace database knows.
    Layout layout("rules_bound");
    auto& project = layout.project;
    ZASSERT(project.cdb.source_count() == 2U);

    auto x = project.entries(layout.fid("lib/x.cpp"));
    ZASSERT(x.size() == 2U);
    ZEXPECT(has_arg(layout.render("lib/x.cpp"), "LIB"));
    ZEXPECT(has_arg(layout.render("src/a.cpp"), "ROOT"));

    /// Edits accumulate from the rules matching the file, headers included.
    auto edits = project.rules.edits({layout.path("lib/y.hxx")}).edits;
    ZASSERT(edits.size() == 1U);
    ZEXPECT(edits[0].kind == CommandEdit::Kind::Append);
    ZEXPECT(edits[0].flags == (std::vector<std::string>{"-x", "c++-header"}));
    ZEXPECT(project.rules.edits({layout.path("lib/x.cpp")}).empty());

    auto members = project.members();
    ZEXPECT(members.size() == 2U);
    for(auto member: members) {
        ZEXPECT(project.rules.indexable(layout.files.resolve(member)));
    }
};

ZEST_CASE(DefaultCommandMembers) {
    /// No database anywhere: the rule's default command serves the files
    /// its patterns cover, enumerates the matching sources as members, and
    /// a nested rule keeps some of them out of the index.
    Layout layout("default_command_only");
    auto& project = layout.project;
    ZEXPECT(project.rules.declared_databases().empty());
    ZEXPECT(project.rules.declares_sources());

    auto commands = project.candidates(layout.fid("src/main.cpp"));
    ZASSERT(commands.size() == 1U);
    ZEXPECT(commands.front().source == CommandSource::Default);
    ZEXPECT(!project.candidates(layout.fid("src/main.cpp")).empty());
    auto rendered = layout.render("src/main.cpp");
    ZEXPECT(has_arg(rendered, "DEFAULTED"));
    ZEXPECT(has_arg(rendered, layout.path("include")));

    /// A file no rule covers has no command; the builtin fallback serves it.
    ZEXPECT(!!project.candidates(layout.fid("tools/other.cpp")).empty());
    ZEXPECT(has_arg(layout.render("tools/other.cpp"), "clang++"));

    auto members = project.members();
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
    ZEXPECT(defaults.project.is_unit(defaults.fid("src/main.cpp")));
    ZEXPECT(!defaults.project.is_unit(defaults.fid("include/lib.h")));
    ZEXPECT(!defaults.project.is_unit(defaults.fid("tools/other.cpp")));

    Layout bound("rules_bound");
    ZEXPECT(bound.project.is_unit(bound.fid("lib/x.cpp")));
    ZEXPECT(!bound.project.is_unit(bound.fid("lib/y.hxx")));
};

#ifndef _WIN32
ZEST_CASE(WalkResolvesLinks) {
    /// A symlink under a pattern root names the file it points to, which
    /// the pattern does not cover.
    TempDir tmp;
    tmp.touch("src/main.cpp", "int main() {}\n");
    tmp.touch("elsewhere/impl.cpp", "");
    ZASSERT(::symlink(tmp.path("elsewhere/impl.cpp").c_str(), tmp.path("src/alias.cpp").c_str()) ==
            0);

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    activate(project);
    auto members = project.members();
    ZASSERT(members.size() == 1U);
    ZEXPECT(files.resolve(members.front()) ==
            CanonicalPath(Spelling::absolute(tmp.path("src/main.cpp"))));
};
#endif

ZEST_CASE(DefaultSourcesSkipNested) {
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

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings = load_settings(project.root).settings;
    project.rules =
        Rules(project.settings, project.root, project.settings.fallback_configuration());
    auto fid = [&](llvm::StringRef name) {
        return files.intern(CanonicalPath(Spelling::absolute(tmp.path(name))));
    };
    auto members = project.members();
    ZEXPECT(ranges::contains(members, fid("main.cpp")));
    ZEXPECT(ranges::contains(members, fid("other/b.cpp")));
    ZEXPECT(!ranges::contains(members, fid("sub/a.cpp")));
    ZEXPECT(!ranges::contains(members, fid("tool/c.cpp")));
};

ZEST_CASE(PatternRootSkipsNested) {
    /// A pattern pointing into a project nested in this one covers none of
    /// its sources: the walk starts below the nested configuration.
    TempDir tmp;
    tmp.touch("clice.toml",
              "[[rules]]\npatterns = [\"sub/src/*.cpp\", \"src/*.cpp\"]\n"
              "default_command = \"clang++ -std=c++20\"\n");
    tmp.touch("src/main.cpp", "int main() {}\n");
    tmp.touch("sub/clice.toml", "");
    tmp.touch("sub/src/a.cpp", "");

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings = load_settings(project.root).settings;
    project.rules =
        Rules(project.settings, project.root, project.settings.fallback_configuration());
    auto fid = [&](llvm::StringRef name) {
        return files.intern(CanonicalPath(Spelling::absolute(tmp.path(name))));
    };
    auto members = project.members();
    ZEXPECT(ranges::contains(members, fid("src/main.cpp")));
    ZEXPECT(!ranges::contains(members, fid("sub/src/a.cpp")));
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

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    auto under_clice = [&](Rule rule) {
        rule.directory = tmp.path(".clice");
        return rule;
    };
    project.settings.rules.push_back(under_clice(
        {.patterns = {"${workspace}/src/**"}, .default_command = std::string("clang++ -DSRC")}));
    project.settings.rules.push_back(under_clice(
        {.patterns = {"../lib/*.cpp"}, .default_command = std::string("clang++ -DLIB")}));
    activate(project);
    auto& compiled = project.settings.compiled_rules;
    ZASSERT(compiled.size() == 2U);
    ZEXPECT(compiled[0].patterns[0].root == CanonicalPath(Spelling::absolute(tmp.path("src"))));
    ZEXPECT(compiled[1].patterns[0].root == CanonicalPath(Spelling::absolute(tmp.path("lib"))));

    auto members = project.members();
    ZASSERT(members.size() == 2U);
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/main.cpp")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("lib/util.cpp")))));
    ZEXPECT(
        project.candidates(files.intern(Spelling::absolute(tmp.path("other/skip.cpp")))).empty());
    auto util = project.candidates(files.intern(Spelling::absolute(tmp.path("lib/util.cpp"))));
    ZASSERT(util.size() == 1U);
    ZEXPECT(has_arg(project.commands.render_full(util.front().base), "LIB"));
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
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(
        Rule{.patterns = {"src/tool"}, .default_command = std::string("clang++ -x c++")});
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++ -x c++-header")});
    project.settings.rules.push_back(Rule{
        .patterns = {"sketch/*.ino", "sketch/**"},
        .default_command = std::string("clang++ -x c++")
    });
    activate(project);

    auto members = project.members();
    ZASSERT(members.size() == 4U);
    ZEXPECT(
        ranges::contains(members, files.intern(Spelling::absolute(tmp.path("sketch/blink.ino")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/tool")))));
    ZEXPECT(ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/pre.i")))));
    ZEXPECT(
        ranges::contains(members, files.intern(Spelling::absolute(tmp.path("src/iface.cppm")))));
};

ZEST_CASE(WorkspaceRuleSkipsUnknown) {
    /// A rule without patterns applies to every file, so its forced language
    /// covers only what clang recognizes as a source: the configuration file
    /// and an extensionless script stay out until a pattern names the script.
    TempDir tmp;
    tmp.touch("clice.toml", "");
    tmp.touch("main.cpp", "");
    tmp.touch("tool", "");
    FileTable files;
    Settings settings;
    settings.rules.push_back(Rule{.default_command = std::string("clang++ -x c++")});
    /// The members of a project opened under `opened`: a project reads its
    /// rules once, when it opens.
    auto members_under = [&](const Settings& opened) {
        Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
        project.settings = opened;
        activate(project);
        return project.members();
    };
    auto members = members_under(settings);
    ZASSERT(members.size() == 1U);
    ZEXPECT(members.front() == files.intern(Spelling::absolute(tmp.path("main.cpp"))));

    settings.rules.insert(
        settings.rules.begin(),
        Rule{.patterns = {"tool"}, .default_command = std::string("clang++ -x c++")});
    members = members_under(settings);
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

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{
        .compile_commands = {"a", "b", "c"}
    });
    activate(project);
    for(auto source: project.rules.declared_databases()) {
        project.cdb.load(source);
    }
    auto members = project.members();
    ZASSERT(members.size() == 1U);
    ZEXPECT(project.entries(members.front()).size() == 3U);
    ZEXPECT(project.scan_commands(members).size() == 2U);
};

ZEST_CASE(CUDAHeaderStaysHeader) {
    /// A `.cuh` under a rule forcing CUDA stays a header: clang's extension
    /// table does not know the suffix, but it names no unit.
    TempDir tmp;
    tmp.touch("cuda/kernel.cu", "");
    tmp.touch("cuda/kernel.cuh", "");
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(
        Rule{.patterns = {"cuda/**"}, .default_command = std::string("clang++ -x cuda")});
    activate(project);
    ZEXPECT(project.members().size() == 1U);
    ZEXPECT(!project.is_unit(files.intern(Spelling::absolute(tmp.path("cuda/kernel.cuh")))));
};

ZEST_CASE(InvalidDefaultCommandIgnored) {
    /// A default command that names no compiler makes no member instead of
    /// aborting: its files have no command of their own, and compile under
    /// the builtin fallback.
    TempDir tmp;
    tmp.touch("main.cpp", "");
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{.default_command = std::string("ccache")});
    activate(project);

    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    ZEXPECT(project.candidates(main).empty());
    ZEXPECT(project.members().empty());
    ZEXPECT(project.fallback_command(CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))) !=
            invalid_command);
};

ZEST_CASE(UnmatchableRuleDeclaresNothing) {
    /// A default-command rule whose every pattern is invalid can cover no
    /// file, so it declares no source and discovery stays on; a database it
    /// names still counts, since entries apply regardless of patterns.
    TempDir tmp;
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}"},
        .default_command = std::string("clang++"),
    });
    activate(project);
    ZEXPECT(!project.rules.declares_sources());
    ZEXPECT(project.members().empty());

    project.settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}"},
        .compile_commands = {"build"},
    });
    activate(project);
    ZEXPECT(project.rules.declares_sources());
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
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    activate(project);
    auto& cdb = project.cdb;
    auto sub = cdb.add_source(Spelling::absolute(tmp.path("sub")));
    auto build_dir = cdb.add_source(Spelling::absolute(tmp.path("build")));
    auto root = cdb.add_source(Spelling::absolute(tmp.path("compile_commands.json")));
    for(auto id: {sub, build_dir, root}) {
        ZASSERT(cdb.load_source(id));
        ZEXPECT(project.discovered(id));
    }

    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    ZEXPECT(project.source_order(files.resolve(main)) ==
            (llvm::SmallVector<SourceID, 4>{root, build_dir, sub}));
    ZEXPECT(project.entries(main).front().source == root);

    cdb.set_present(root, false);
    ZEXPECT(project.source_order(files.resolve(main)) ==
            (llvm::SmallVector<SourceID, 4>{build_dir, sub, root}));
    ZEXPECT(project.entries(main).front().source == build_dir);
};

#ifndef _WIN32
ZEST_CASE(LinkedBuildFollowsLink) {
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
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(
        Rule{.patterns = {"src/**"}, .default_command = std::string("clang++")});
    activate(project);
    auto refresh = [&] {
        return project.refresh_default_sources(walk_sources(project.rules.source_walk()));
    };
    ZASSERT(project.members().size() == 1u);
    ZEXPECT(refresh().empty());

    tmp.touch("src/later.cpp", "");
    auto later = files.intern(Spelling::absolute(tmp.path("src/later.cpp")));
    ZEXPECT(refresh() == llvm::SmallVector<Fid>{later});
    ZEXPECT(refresh().empty());
    ZEXPECT(project.members().size() == 2u);

    fs::remove_all(tmp.path("src/later.cpp"));
    ZEXPECT(refresh().empty());
    ZEXPECT(project.members().size() == 1u);
};

ZEST_CASE(DeclaredSourceOffDiscovery) {
    /// A rule declaring a default command is the whole intent: the database
    /// sitting at the root is not consulted.
    Layout layout("declared_ignores_discovered");
    ZEXPECT(layout.project.rules.declares_sources());
    ZEXPECT(layout.project.rules.declared_databases().empty());
    ZEXPECT(layout.project.cdb.source_count() == 0U);
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

    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.default_configuration = "release";
    project.settings.rules.push_back(Rule{.configuration = "debug",
                                          .compile_commands = {"debug"},
                                          .append = {"-DFROM_DEBUG_RULE"}});
    project.settings.rules.push_back(
        Rule{.configuration = "release", .compile_commands = {"release"}});
    activate(project);
    ZEXPECT(project.rules.configuration() == "release");
    ZASSERT(project.rules.declared_databases().size() == 1U);
    project.cdb.load(tmp.path("release"));
    project.cdb.load(tmp.path("debug"));
    ZASSERT(project.cdb.source_count() == 2U);

    auto main = files.intern(Spelling::absolute(tmp.path("main.cpp")));
    auto candidates = project.entries(main);
    ZASSERT(candidates.size() == 1U);
    ZEXPECT(has_arg(project.commands.render_full(candidates.front().command), "RELEASE"));
    ZEXPECT(project.rules.edits({CanonicalPath(Spelling::absolute(tmp.path("main.cpp")))}).empty());
};

ZEST_CASE(InactiveSourceKeepsDiscovery) {
    /// Only an inactive configuration declares a database: the active one
    /// has no source, so discovery stays on for it.
    TempDir tmp;
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.default_configuration = "release";
    project.settings.rules.push_back(Rule{.configuration = "debug", .compile_commands = {"."}});
    project.settings.rules.push_back(Rule{.configuration = "release", .append = {"-DNDEBUG"}});
    activate(project);
    ZEXPECT(!project.rules.declares_sources());
    ZEXPECT(project.rules.declared_databases().empty());

    /// The database the inactive configuration names is the one discovery
    /// finds at the root: registered, it serves as a discovered source
    /// rather than hiding behind the inactive declaration.
    tmp.touch("main.cpp", "");
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("main.cpp"), {}}
    }));
    ZEXPECT(project.entries(files.intern(Spelling::absolute(tmp.path("main.cpp")))).size() == 1U);
    ZEXPECT(project.members().size() == 1U);
};

ZEST_CASE(RuleTurnsQueryOff) {
    /// A matching rule turning the toolchain query off turns it off for the
    /// file's command, alone or beside other edits, and the edit hash says
    /// so; a command borrowed from such a file keeps it off.
    TempDir tmp;
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{
        .patterns = {"src/**", "vendor/**"},
        .append = {"-DA"}
    });
    project.settings.rules.push_back(Rule{
        .patterns = {"vendor/**", "third_party/**"},
        .query_toolchain = false
    });
    activate(project);

    CanonicalPath own(Spelling::absolute(tmp.path("src/a.cpp")));
    CanonicalPath vendor(Spelling::absolute(tmp.path("vendor/b.cpp")));
    CanonicalPath third_party(Spelling::absolute(tmp.path("third_party/c.cpp")));
    CanonicalRef own_file = own;
    CanonicalRef vendor_file = vendor;
    CanonicalRef third_party_file = third_party;
    ZEXPECT(project.rules.edits({own_file}).query_toolchain);
    ZEXPECT(!project.rules.edits({vendor_file}).query_toolchain);
    ZEXPECT(!project.rules.edits({third_party_file}).query_toolchain);

    // One command under the same appends: only the query tells them apart.
    auto command = std::format("clang++ {}", vendor.str());
    auto entry = *project.cdb.add_command(tmp.root, vendor.str(), llvm::StringRef(command));
    auto unqueried =
        project.apply_rules(entry.file, {.base = entry.command, .source = CommandSource::Database});
    auto queried = project.apply_rules(files.intern(own_file),
                                       {.base = entry.command, .source = CommandSource::Database});
    ZEXPECT(!project.commands.get(unqueried.command).query_toolchain);
    ZEXPECT(project.commands.get(queried.command).query_toolchain);
    ZEXPECT(project.commands.command_hash(unqueried.command) !=
            project.commands.command_hash(queried.command));

    auto borrowed = project.apply_rules(
        files.intern(own_file),
        {.base = entry.command, .source = CommandSource::Borrowed, .rules_from = {entry.file}});
    ZEXPECT(!project.commands.get(borrowed.command).query_toolchain);
};

ZEST_CASE(MembersFollowCommands) {
    /// A change of the build's commands: a unit the database stopped
    /// listing, which nothing else covers, leaves the index, its pending
    /// reindex forgotten; a unit whose command changed keeps its index,
    /// outdated, and owes the reindex of a content change. Every change
    /// moves the commands' epoch on once; none changes nothing.
    TempDir tmp;
    tmp.touch("kept.cpp", "int kept;\n");
    tmp.touch("gone.cpp", "int gone;\n");
    IndexerFixture f;
    auto& project = f.project;
    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("kept.cpp"), {}},
                  {tmp.root, tmp.path("gone.cpp"), {}},
    }));
    auto kept = project.file_table.intern(Spelling::absolute(tmp.path("kept.cpp")));
    auto gone = project.file_table.intern(Spelling::absolute(tmp.path("gone.cpp")));
    for(auto unit: {kept, gone}) {
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = project.file_table.intern_version(unit, 1);
        project.index.apply_manifest(unit, std::move(manifest));
    }
    ZASSERT(project.background_index.enqueue(gone, ReindexReason::DepsOnly));

    write_cdb(tmp,
              project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("kept.cpp"), {"-DCHANGED"}}
    }));
    auto epoch = project.commands_epoch;
    ZEXPECT(!project.update_members({.changed = {kept}, .removed = {gone}}).commands.empty());
    ZEXPECT(project.commands_epoch == epoch + 1);
    ZEXPECT(!project.index.indexed(gone));
    ZEXPECT(!f.queue.pending_reason(gone).has_value());
    ZEXPECT(project.index.indexed(kept));
    ZEXPECT(IndexInternals::outdated(project.index, kept));
    ZEXPECT(f.queue.pending_reason(kept) == ReindexReason::ContentChanged);

    auto graph = project.dep_graph.generation();
    ZEXPECT(project.update_members({}).commands.empty());
    ZEXPECT(project.commands_epoch == epoch + 1);
    ZEXPECT(project.dep_graph.generation() == graph);
    ZEXPECT(IndexerFixture::pending_files(f.queue) == llvm::SmallVector<Fid>{kept});
}

ZEST_CASE(BorrowMergesRules) {
    /// A borrowed command is edited by the rules matching the file it is
    /// from and the borrower, in declaration order, a rule matching both
    /// once.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{.patterns = {"new/**"}, .append = {"-DOWN"}});
    project.settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DUNIT"}});
    project.settings.rules.push_back(Rule{.patterns = {"**/*"}, .append = {"-DBOTH"}});
    activate(project);
    auto command = std::format("clang++ {}", tmp.path("src/a.cpp"));
    auto unit = *project.cdb.add_command(tmp.root, tmp.path("src/a.cpp"), llvm::StringRef(command));

    auto ref = project.apply_rules(
        files.intern(Spelling::absolute(tmp.path("new/n.cpp"))),
        {.base = unit.command, .source = CommandSource::Borrowed, .rules_from = {unit.file}});
    auto argv = project.commands.render_driver(ref, project.cdb.spelling(ref.file));
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
    ZEXPECT(count("UNIT") == 1);
    ZEXPECT(count("OWN") == 1);
    ZEXPECT(count("BOTH") == 1);
    ZEXPECT(at("OWN") < at("UNIT"));
    ZEXPECT(at("UNIT") < at("BOTH"));
};

ZEST_CASE(MemberHostKeepsRules) {
    /// A header compiling in a provisional member's context compiles as the
    /// member does: under the rules of the unit it borrows from and its own,
    /// and the header's.
    TempDir tmp;
    tmp.touch("src/a.cpp", "");
    tmp.touch("new/b.cpp", "");
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DUNIT"}});
    project.settings.rules.push_back(Rule{.patterns = {"new/*.cpp"}, .append = {"-DHOST"}});
    project.settings.rules.push_back(Rule{.patterns = {"**/*.h"}, .append = {"-DHEADER"}});
    activate(project);
    auto command = std::format("clang++ {}", tmp.path("src/a.cpp"));
    auto unit = *project.cdb.add_command(tmp.root, tmp.path("src/a.cpp"), llvm::StringRef(command));
    auto member = files.intern(Spelling::absolute(tmp.path("new/b.cpp")));
    ZASSERT(project.record_provisional(member));
    project.provisional.set_command(member,
                                    Candidate{.base = unit.command,
                                              .source = CommandSource::Borrowed,
                                              .rules_from = {unit.file}});

    auto header = files.intern(Spelling::absolute(tmp.path("new/h.h")));
    auto ref = project.apply_rules(
        header,
        project.candidates(member).front().passed_on(member, CommandSource::Host));
    auto argv = project.commands.render_full(ref.command);
    ZEXPECT(has_arg(argv, "UNIT"));
    ZEXPECT(has_arg(argv, "HOST"));
    ZEXPECT(has_arg(argv, "HEADER"));
};

ZEST_CASE(PCHTakesBorrowedRules) {
    /// The precompiled header a borrowed command reads builds under the
    /// rules that edited the command: the unit's as well as the
    /// borrower's.
    TempDir tmp;
    tmp.touch("pch.hxx", "");
    tmp.touch("pch.hxx.cxx", "");
    tmp.touch("src/a.cpp", "");
    FileTable files;
    Project project{files, CanonicalPath(Spelling::absolute(tmp.root))};
    project.settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DUNIT"}});
    activate(project);
    auto pch = tmp.path("pch.hxx.pch");
    auto writes = std::format("clang++ -Xclang -emit-pch -x c++-header -o {} {}",
                              pch,
                              tmp.path("pch.hxx.cxx"));
    project.cdb.add_command(tmp.root, tmp.path("pch.hxx.cxx"), llvm::StringRef(writes));
    auto reads =
        std::format("clang++ -Xclang -include-pch -Xclang {} {}", pch, tmp.path("src/a.cpp"));
    auto unit = *project.cdb.add_command(tmp.root, tmp.path("src/a.cpp"), llvm::StringRef(reads));

    auto borrower = files.intern(Spelling::absolute(tmp.path("new/n.cpp")));
    Candidate borrowed{.base = unit.command,
                       .source = CommandSource::Borrowed,
                       .rules_from = {unit.file}};
    SelectedCommand selection;
    selection.candidate = borrowed;
    selection.ref = project.apply_rules(borrower, borrowed);
    auto writer = project.pch_writer(borrower, selection);
    ZASSERT(writer);
    ZEXPECT(has_arg(project.commands.render_full(writer->command), "UNIT"));
};

/// The builtin fallback render for a file without an entry and no default
/// command, resource dir stripped like render_entry.
std::vector<const char*> render_fallback(Project& project,
                                         llvm::StringRef file,
                                         const CommandEdits& options = {}) {
    auto applied = apply_edits(project.commands,
                               project.file_table,
                               project.fallback_command(CanonicalPath(Spelling::absolute(file))),
                               options);
    CommandRef ref{project.file_table.intern(Spelling::absolute(file)),
                   applied,
                   project.commands.input_kind(applied, file),
                   CommandSource::Fallback};
    auto argv = project.commands.render_driver(ref, project.cdb.spelling(ref.file));
    for(std::size_t i = 0; i + 1 < argv.size(); i += 1) {
        if(llvm::StringRef(argv[i]) == "-resource-dir") {
            argv.erase(argv.begin() + i, argv.begin() + i + 2);
            break;
        }
    }
    return argv;
}

ZEST_CASE(DefaultFallback) {
    FileTable files;
    Project project{files};

    /// C++ files get "clang++ -std=c++20 <file>".
    auto cpp_argv = render_fallback(project, fake("unknown.cpp"));
    ZASSERT(cpp_argv.size() == 3U);
    ZEXPECT(cpp_argv[0] == "clang++"sv);
    ZEXPECT(cpp_argv[1] == "-std=c++20"sv);
    ZEXPECT(cpp_argv[2] == fake("unknown.cpp"));

    /// Every extension clang classifies as C++ gets the C++ default.
    for(llvm::StringRef cxx_file: {"header.hpp", "file.cc", "file.cxx", "file.C", "file.hh"}) {
        ZEXPECT(render_fallback(project, fake(cxx_file))[0] == "clang++"sv);
    }

    /// C files get "clang <file>".
    auto c_argv = render_fallback(project, fake("unknown.c"));
    ZASSERT(c_argv.size() == 2U);
    ZEXPECT(c_argv[0] == "clang"sv);
    ZEXPECT(c_argv[1] == fake("unknown.c"));

    /// An ambiguous header is C++ by default, forced through -x so it
    /// compiles as a translation unit; other extensions get plain clang.
    auto h_argv = render_fallback(project, fake("foo.h"));
    ZASSERT(h_argv.size() == 5U);
    ZEXPECT(h_argv[0] == "clang++"sv);
    ZEXPECT(h_argv[2] == "-x"sv);
    ZEXPECT(h_argv[3] == "c++"sv);
    ZEXPECT(render_fallback(project, fake("foo.m"))[0] == "clang"sv);

    /// CUDA files pin cuda mode and the device-side view NVCC-backed
    /// commands default to.
    for(llvm::StringRef cuda_file: {"kern.cu", "kernels.cuh"}) {
        auto cu_argv = render_fallback(project, fake(cuda_file));
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
    FileTable files;
    Project project{files};
    std::vector<CommandEdit> edits = {
        {CommandEdit::Kind::Append, {"-I/opt/include"}}
    };
    CommandEdits options{.edits = edits};

    auto argv = join_command(render_fallback(project, fake("unknown.cpp"), options));
    ZEXPECT(llvm::StringRef(argv).contains("-std=c++20"));
    ZEXPECT(llvm::StringRef(argv).contains("/opt/include"));

    /// The plain-clang branch applies them too.
    ZEXPECT(llvm::StringRef(join_command(render_fallback(project, fake("unknown.c"), options)))
                .contains("/opt/include"));
};

};  // ZEST_SUITE(Members)

}  // namespace

}  // namespace clice::testing
