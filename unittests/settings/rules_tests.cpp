module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :command.edit;
import :settings.rules;
import :settings.settings;
import :unittests.utils;
import :unittests.utils.cdb_layouts;
import :unittests.utils.temp_dir;
import :vfs.path;

namespace clice::testing {

namespace {

/// One of the layouts written to a temporary directory: its clice.toml
/// loaded the way the server loads it, its rules under the configuration
/// the server activates.
struct Layout {
    TempDir dir;
    std::string root;
    CanonicalPath project_root;
    Settings settings;
    Rules rules;

    explicit Layout(llvm::StringRef name) :
        root(write_layout(dir, name)), project_root(Spelling::absolute(root)),
        settings(load_settings(project_root).settings),
        rules(settings, project_root, settings.fallback_configuration()) {}

    /// The file's identity, like every path the build hands out.
    CanonicalPath path(llvm::StringRef relative) const {
        return CanonicalPath(Spelling::absolute(path::join(root, relative)));
    }
};

ZEST_SUITE(Rules) {

ZEST_CASE(LintSet) {
    /// Files outside the workspace are never linted; inside it, every
    /// matching rule must keep `lint` on.
    Layout layout("lint_rules");
    ZEXPECT(layout.rules.lintable(layout.path("src/main.cpp")));
    ZEXPECT(layout.rules.lintable(layout.path("include/api.h")));
    ZEXPECT(!layout.rules.lintable(layout.path("vendor/lib.cpp")));
    ZEXPECT(!layout.rules.lintable(layout.path("vendor/deep/lib.h")));
    ZEXPECT(!layout.rules.lintable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
    ZEXPECT(
        !layout.rules.lintable(CanonicalPath(Spelling::absolute(layout.root + "-sibling/x.cpp"))));
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
    Rules rules(settings, project_root, settings.fallback_configuration());
    ZEXPECT(rules.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/src/main.cpp")))));
    ZEXPECT(!rules.lintable(CanonicalPath(Spelling::absolute(tmp.path("link/vendor/lib.cpp")))));
    ZEXPECT(!rules.lintable(CanonicalPath(Spelling::absolute(tmp.path("elsewhere/x.cpp")))));
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
    Rules rules(settings, project_root, settings.fallback_configuration());
    ZEXPECT(!rules.lintable(CanonicalPath(Spelling::absolute(tmp.path("vendor/lib.cpp")))));
};
#endif

ZEST_CASE(FormatSet) {
    /// The format set reads its own rule field: a `lint = false` directory
    /// still formats, a `format = false` one does not.
    Layout layout("lint_rules");
    ZEXPECT(layout.rules.formattable(layout.path("src/main.cpp")));
    ZEXPECT(layout.rules.formattable(layout.path("vendor/lib.cpp")));
    ZEXPECT(!layout.rules.formattable(layout.path("gen/out.h")));
    ZEXPECT(layout.rules.lintable(layout.path("gen/out.h")));
    ZEXPECT(!layout.rules.formattable(CanonicalPath(Spelling::absolute("/usr/include/stdio.h"))));
};

ZEST_CASE(HeaderCarriesHostEdits) {
    /// A header borrowing a host command carries both files' edits, each
    /// rule once, in declaration order.
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DA"}});
    settings.rules.push_back(Rule{.patterns = {"include/**"}, .append = {"-DB"}});
    settings.rules.push_back(Rule{.patterns = {"**/*"}, .append = {"-DC"}, .remove = {"-DA"}});
    CanonicalPath project_root = CanonicalPath(Spelling::absolute(tmp.root));
    settings.finalize(project_root);
    Rules rules(settings, project_root, "");

    CanonicalPath host(Spelling::absolute(tmp.path("src/main.cpp")));
    CanonicalPath header(Spelling::absolute(tmp.path("include/x.h")));
    CanonicalRef both[] = {host, header};
    auto edits = rules.edits(both).edits;
    ZASSERT(edits.size() == 4U);
    ZEXPECT(edits[0].flags == (std::vector<std::string>{"-DA"}));
    ZEXPECT(edits[1].flags == (std::vector<std::string>{"-DB"}));
    ZEXPECT(edits[2].kind == CommandEdit::Kind::Remove);
    ZEXPECT(edits[2].flags == (std::vector<std::string>{"-DA"}));
    ZEXPECT(edits[3].flags == (std::vector<std::string>{"-DC"}));

    auto header_only = rules.edits({header}).edits;
    ZASSERT(header_only.size() == 3U);
    ZEXPECT(header_only[0].flags == (std::vector<std::string>{"-DB"}));
};

};  // ZEST_SUITE(Rules)

ZEST_SUITE(DatabaseKey) {

ZEST_CASE(Resolved) {
    /// A configuration resolving symlinks keeps a database per real path
    /// its databases resolve to — the own one of a configuration declaring
    /// exactly those — and any other configuration its own.
    TempDir tmp;
    tmp.touch("out/a/compile_commands.json", "[]");
    tmp.touch("out/b/compile_commands.json", "[]");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto configured = [&](llvm::StringRef followed) {
        Settings settings;
        settings.rules.push_back(Rule{.configuration = "active",
                                      .compile_commands = {followed.str()},
                                      .resolve_symlinks = true});
        settings.rules.push_back(Rule{.configuration = "a", .compile_commands = {"out/a"}});
        settings.rules.push_back(Rule{
            .configuration = "both",
            .compile_commands = {"out/a", "out/b"}
        });
        settings.finalize(root);
        return settings;
    };
    auto key_of = [&](const Settings& settings, llvm::StringRef configuration) {
        Rules rules(settings, root, configuration);
        return rules.database_key(rules.resolved_databases());
    };
    auto settings = configured("out/a");
    ZEXPECT(key_of(settings, "active").str() == "a");
    ZEXPECT(key_of(settings, "a").str() == "a");
    ZEXPECT(key_of(settings, "both").str() == "both");

    settings = configured("out/b");
    auto key = key_of(settings, "active");
    ZEXPECT(key.configuration == "active");
    ZEXPECT(key.resolved ==
            CanonicalPath(Spelling::absolute(tmp.path("out/b/compile_commands.json"))).str());

    // Naming the databases is not all it does: another build.
    settings = configured("out/a");
    settings.rules[1].append = {"-DTRACE"};
    settings.finalize(root);
    ZEXPECT(key_of(settings, "active").configuration == "active");
};

};  // ZEST_SUITE(DatabaseKey)

}  // namespace

}  // namespace clice::testing
