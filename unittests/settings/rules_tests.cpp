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
    /// Files outside the workspace are never linted; inside it, the first
    /// matching rule setting `lint` decides.
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

ZEST_CASE(PatternMatchesBuildPath) {
    /// Bazel links an external repository from its cache: a `**` pattern
    /// naming external/ matches the path the build reaches a file by, though
    /// the file's identity lies in the cache.
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
patterns = ["**/external/**"]
lint = false
)");
    tmp.touch("cache/kotatsu/a.cpp", "int a() { return 0; }\n");
    tmp.mkdir("external");
    ZASSERT(::symlink(tmp.path("cache/kotatsu").c_str(), tmp.path("external/kotatsu").c_str()) ==
            0);

    CanonicalPath project_root(Spelling::absolute(tmp.root));
    Settings settings = load_settings(project_root).settings;
    Rules rules(settings, project_root, settings.fallback_configuration());
    auto reached = Spelling::absolute(tmp.path("external/kotatsu/a.cpp"));
    CanonicalPath identity(reached);
    ZEXPECT(rules.lintable(identity));
    ZEXPECT(!rules.lintable({identity, reached}));
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

ZEST_CASE(FirstSetFieldDecides) {
    /// Of the rules matching a file, the first to set a field decides it:
    /// a narrow rule before a broad one, and a field the narrow rule leaves
    /// out falls through to the broad one; one no rule sets keeps its
    /// default.
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
patterns = ["vendor/**"]
background_index = false
readonly = "on"
query_toolchain = true

[[rules]]
background_index = true
readonly = "auto"
query_toolchain = false
lint = false
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.issues.empty());
    Rules rules(loaded.settings, root, "");
    CanonicalPath vendor(Spelling::absolute(tmp.path("vendor/lib.cpp")));
    CanonicalPath own(Spelling::absolute(tmp.path("src/main.cpp")));

    ZEXPECT(!rules.indexable(vendor));
    ZEXPECT(rules.indexable(own));
    ZEXPECT(rules.readonly(vendor) == ReadonlyMode::On);
    ZEXPECT(rules.readonly(own) == ReadonlyMode::Auto);
    ZEXPECT(rules.edits({vendor}).query_toolchain);
    ZEXPECT(!rules.edits({own}).query_toolchain);
    ZEXPECT(!rules.lintable(vendor));
    ZEXPECT(rules.formattable(vendor));
    ZEXPECT(rules.other_contexts(vendor));
    ZEXPECT(!rules.self_contained(vendor).has_value());

    /// The rule deciding names where it is written.
    auto file = path::join(root, "clice.toml");
    auto* indexing = rules.deciding(vendor, &CompiledRule::background_index);
    ZASSERT(indexing != nullptr);
    ZEXPECT(indexing->origin == file + ":1");
    auto* linting = rules.deciding(vendor, &CompiledRule::lint);
    ZASSERT(linting != nullptr);
    ZEXPECT(linting->origin == file + ":7");
    ZEXPECT(rules.deciding(vendor, &CompiledRule::format) == nullptr);
};

ZEST_CASE(BodyRulesInOrder) {
    /// The active rules saying something of function bodies, in order,
    /// their patterns as compiled: anchored at the project root.
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"third_party/**"}, .skip_function_body = true});
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DX"}});
    settings.rules.push_back(Rule{.configuration = "release", .index_function_body = false});
    settings.rules.push_back(Rule{.configuration = "debug", .append = {"-DDEBUG"}});
    settings.rules.push_back(Rule{.index_function_body = true});
    CanonicalPath root(Spelling::absolute(tmp.root));
    settings.finalize(root);
    Rules rules(settings, root, "debug");

    auto bodies = rules.function_body_rules();
    ZASSERT(bodies.rules.size() == 2u);
    auto& skipped = bodies.rules[0];
    ZEXPECT(skipped.patterns ==
            (std::vector<std::string>{settings.compiled_rules[0].patterns[0].text}));
    ZEXPECT(llvm::StringRef(skipped.patterns[0]).ends_with("/third_party/**"));
    ZEXPECT(skipped.skip == true);
    ZEXPECT(!skipped.index.has_value());
    auto& indexed = bodies.rules[1];
    ZEXPECT(indexed.patterns.empty());
    ZEXPECT(!indexed.skip.has_value());
    ZEXPECT(indexed.index == true);
};

ZEST_CASE(ParsesEveryBody) {
    /// Every body is parsed only when a rule for every file says no skip
    /// and no rule before it skips some files' bodies.
    TempDir tmp;
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto parses_every_body = [&](std::vector<Rule> written) {
        Settings settings;
        settings.rules = std::move(written);
        settings.finalize(root);
        return Rules(settings, root, "").parses_every_body();
    };
    ZEXPECT(!parses_every_body({
        Rule{.patterns = {"vendor/**"}, .skip_function_body = true},
        Rule{.skip_function_body = false},
    }));
    ZEXPECT(parses_every_body({Rule{.skip_function_body = false}}));
    ZEXPECT(!parses_every_body({
        Rule{.patterns = {"src/**"}, .skip_function_body = false}
    }));
    ZEXPECT(!parses_every_body({}));
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
    RulePath both[] = {host, header};
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
