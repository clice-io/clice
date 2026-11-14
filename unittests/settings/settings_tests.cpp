module clice;

import :command.argument;
import :command.database;
import :command.edit;
import :command.table;
import :feature.code_completion;
import :settings.rules;
import :settings.settings;
import :unittests.utils.cdb_helper;
import :unittests.utils.platform;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

/// The schema object of a property named `name`, found anywhere in the
/// document's nested `properties` maps.
const static codec::dyn::Value* find_property(const codec::dyn::Value& value,
                                              std::string_view name) {
    if(const auto* object = value.get_object()) {
        for(const auto& [key, child]: *object) {
            if(key == "properties") {
                if(const auto* properties = child.get_object()) {
                    if(const auto* found = properties->find(name)) {
                        return found;
                    }
                }
            }
            if(const auto* found = find_property(child, name)) {
                return found;
            }
        }
    } else if(const auto* array = value.get_array()) {
        for(const auto& child: *array) {
            if(const auto* found = find_property(child, name)) {
                return found;
            }
        }
    }
    return nullptr;
}

/// Whether `name` appears as an object key anywhere below a `default`
/// annotation in the schema document.
static bool default_mentions(const codec::dyn::Value& value,
                             std::string_view name,
                             bool under_default) {
    if(const auto* object = value.get_object()) {
        for(const auto& [key, child]: *object) {
            if(under_default && key == name) {
                return true;
            }
            if(default_mentions(child, name, under_default || key == "default")) {
                return true;
            }
        }
    } else if(const auto* array = value.get_array()) {
        for(const auto& child: *array) {
            if(default_mentions(child, name, under_default)) {
                return true;
            }
        }
    }
    return false;
}

/// The edits the rules of `settings` contribute for `path`.
void match_rules(const Settings& settings,
                 llvm::StringRef path,
                 std::vector<std::string>& append,
                 std::vector<std::string>& remove) {
    Rules rules(settings, CanonicalPath(), "");
    for(auto& edit: rules.edits({CanonicalPath(Spelling::absolute(path))}).edits) {
        auto& out = edit.kind == CommandEdit::Kind::Remove ? remove : append;
        out.insert(out.end(), edit.flags.begin(), edit.flags.end());
    }
}

/// The overlay a client sends as initializationOptions, from its JSON.
static codec::dyn::Value overlay(std::string_view text) {
    auto value = json::from_string<codec::dyn::Value>(text);
    ZASSERT(value);
    return std::move(*value);
}

ZEST_SUITE(Settings) {

ZEST_CASE(CacheServesOneProject) {
    /// A shared cache directory keeps the root it was first used for; the
    /// next project moves off it to its own default.
    TempDir tmp;
    tmp.touch("shared/keep", "");
    tmp.touch("a/main.cpp", "");
    tmp.touch("b/main.cpp", "");
    auto shared = tmp.path("shared");
    CanonicalPath a(Spelling::absolute(tmp.path("a")));
    CanonicalPath b(Spelling::absolute(tmp.path("b")));
    ZEXPECT(!owned_elsewhere(shared, b));
    // The owner is recorded as its user spells it.
    auto shown = tmp.path("a");
    set_cache_dir_owner(shared, a, shown);
    ZEXPECT(cache_dir_owner(shared) == shown);
    ZEXPECT(!owned_elsewhere(shared, a));
    ZEXPECT(owned_elsewhere(shared, b));

    // One inside its root belongs to that root, whatever it records, and
    // a subproject nested there finds it taken once the root became its owner.
    tmp.touch("a/.clice/owner", b.str() + "\n");
    auto inner = path::join(a, ".clice");
    ZEXPECT(!owned_elsewhere(inner, a));
    set_cache_dir_owner(inner, a, a);
    ZEXPECT(owned_elsewhere(inner, CanonicalPath(Spelling::absolute(path::join(a, "sub")))));

    // An owner that no longer exists owns nothing.
    tmp.touch("moved/owner", tmp.path("gone") + "\n");
    auto moved = tmp.path("moved");
    ZEXPECT(!owned_elsewhere(moved, b));
    set_cache_dir_owner(moved, b, b);
    ZEXPECT(owned_elsewhere(moved, a));

    tmp.touch("b/clice.toml", std::format("cache_dir = '{}'\n", shared));
    auto loaded = load_settings(b);
    auto own = path::join(b, ".clice");
    path::canonicalize(own);
    ZEXPECT(std::string(loaded.settings.cache_dir) == own);
};

ZEST_CASE(CacheDirFallbackChain) {
    TempDir tmp;
    tmp.touch("clice.toml", R"(cache_dir = "file-cache"
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto identity = [&](llvm::StringRef relative) {
        return CanonicalPath(Spelling::absolute(tmp.path(relative)));
    };
    auto cache_overlay = overlay(R"({ "cache_dir": "overlay-cache" })");

    auto merged = load_settings(root, {.overlay = cache_overlay});
    ZEXPECT(std::string_view(merged.settings.cache_dir) == identity("overlay-cache").str());

    std::vector<CanonicalPath> taken = {identity("overlay-cache")};
    auto own = load_settings(root, {.overlay = cache_overlay, .taken_cache_dirs = taken});
    ZEXPECT(std::string_view(own.settings.cache_dir) == identity("file-cache").str());
    ZEXPECT(own.issues.empty());

    taken.push_back(identity("file-cache"));
    auto fallback = load_settings(root, {.overlay = cache_overlay, .taken_cache_dirs = taken});
    ZEXPECT(std::string_view(fallback.settings.cache_dir) == identity(".clice").str());
    ZEXPECT(fallback.settings.cache_dir_defaulted == true);
    ZEXPECT(fallback.issues.empty());
}

ZEST_CASE(ParsePartialWorkers) {
    // A partial decode only touches the fields it names; everything else
    // keeps the field-initializer defaults.
    auto result = codec::toml::from_string<WorkersSettings>("foreground = 3");
    ZASSERT(result);
    ZEXPECT(result->foreground == 3u);
    ZEXPECT(std::holds_alternative<Automatic>(result->background));
    ZEXPECT(result->background_delay_ms == 3000u);
}

ZEST_CASE(ParseRule) {
    auto result = codec::toml::from_string<Rule>(R"(
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    ZEXPECT(result);
    ZEXPECT(result->patterns.size() == 1u);
    ZEXPECT(result->patterns[0] == "**/*.cpp");
    ZEXPECT(result->append[0] == "-std=c++20");
    ZEXPECT(result->remove.empty());
}

ZEST_CASE(ParseFullSettings) {
    auto result = codec::toml::from_string<Settings>(R"(
cache_dir = "/tmp/test"

[workers]
foreground = 4

[[rules]]
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->cache_dir) == "/tmp/test");
    ZEXPECT(result->workers.foreground == 4u);
    ZEXPECT(result->rules.size() == 1u);
    ZEXPECT(result->rules[0].patterns[0] == "**/*.cpp");
}

ZEST_CASE(ParseInlayHints) {
    auto result = codec::toml::from_string<Settings>(R"(
[inlay_hints]
block_end = true
parameters = false

[inlay_hints.max_length]
types = 64
)");
    ZEXPECT(result);
    ZEXPECT(result->inlay_hints.block_end == true);
    ZEXPECT(result->inlay_hints.parameters == false);
    ZEXPECT(at_most(result->inlay_hints.max_length.types) == 64u);
    ZEXPECT(at_most(result->inlay_hints.max_length.block_end) == 60u);
    ZEXPECT(result->inlay_hints.designators == true);
}

ZEST_CASE(ParseEmptySettings) {
    auto result = codec::toml::from_string<Settings>("");
    ZEXPECT(result);
    ZEXPECT(result->rules.empty());
    ZEXPECT(std::string_view(result->cache_dir).empty());
}

ZEST_CASE(ParseOnlyRules) {
    auto result = codec::toml::from_string<Settings>(R"(
[[rules]]
patterns = ["*.h"]
remove = ["-Werror"]
)");
    ZEXPECT(result);
    ZEXPECT(result->rules.size() == 1u);
    ZEXPECT(result->rules[0].patterns[0] == "*.h");
    ZEXPECT(result->rules[0].remove[0] == "-Werror");
    ZEXPECT(std::string_view(result->cache_dir).empty());
}

ZEST_CASE(MatchRulesBasic) {
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-std=c++20"},
        .remove = {"-std=c++17"},
    });
    settings.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(settings, "/src/foo.cpp", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-std=c++20");
    ZEXPECT(remove.size() == 1u);
    ZEXPECT(remove[0] == "-std=c++17");
}

ZEST_CASE(MatchRulesNoMatch) {
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO"},
    });
    settings.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(settings, "/src/foo.h", append, remove);
    ZEXPECT(append.empty());
    ZEXPECT(remove.empty());
}

ZEST_CASE(MatchRulesMultiple) {
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-DCPP"},
    });
    settings.rules.push_back(Rule{
        .patterns = {"**/test_*.cpp"},
        .append = {"-DTEST"},
    });
    settings.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(settings, "/src/test_foo.cpp", append, remove);
    ZEXPECT(append.size() == 2u);
    ZEXPECT(append[0] == "-DCPP");
    ZEXPECT(append[1] == "-DTEST");
}

ZEST_CASE(BornValidDefaults) {
    // A default-constructed Settings is fully valid without any init step;
    // the option defaults come from the field initializers alone.
    Settings settings;
    ZEXPECT(settings.workers.foreground == 2u);
    ZEXPECT(std::holds_alternative<Automatic>(settings.workers.background));
    ZEXPECT(std::holds_alternative<Automatic>(settings.workers.max));
    ZEXPECT(settings.workers.background_count() == default_stateless_worker_count());
    ZEXPECT(settings.workers.background_count() >= 2u);
    ZEXPECT(settings.workers.max_count() == default_max_stateless_worker_count());
    ZEXPECT(settings.workers.background_delay_ms == 3000u);
    ZEXPECT(settings.poll_seconds == 30u);
    ZEXPECT(settings.inlay_hints.enabled == true);
    ZEXPECT(settings.inlay_hints.parameters == true);
    ZEXPECT(settings.inlay_hints.deduced_types == true);
    ZEXPECT(settings.inlay_hints.designators == true);
    ZEXPECT(settings.inlay_hints.block_end == false);
    ZEXPECT(settings.inlay_hints.default_arguments == false);
    ZEXPECT(at_most(settings.inlay_hints.max_length.types) == 32u);
    ZEXPECT(settings.code_completion.arguments == feature::CompletionArguments::Placeholders);
    ZEXPECT(settings.code_completion.bundle_overloads == true);
    ZEXPECT(settings.code_completion.statement_snippets == false);
    ZEXPECT(settings.code_completion.dot_to_arrow == true);
    ZEXPECT(settings.code_completion.all_scopes == true);
    ZEXPECT(settings.code_completion.insert_includes == true);
    ZEXPECT(settings.code_completion.insert_imports == true);
    ZEXPECT(at_most(settings.code_completion.limit) == 100u);
    ZEXPECT(at_most(settings.references.limit) == 2000u);
}

ZEST_CASE(FinalizeDerivesPaths) {
    Settings settings;
    CanonicalPath root(Spelling::absolute("/workspace"));
    settings.finalize(root);
    auto spelled = [&](llvm::StringRef relative) {
        return CanonicalPath(Spelling::absolute(path::join(root, relative))).str();
    };
    ZEXPECT(settings.cache_dir == spelled(".clice"));
    ZEXPECT(settings.logging_dir == spelled(".clice/logs"));
    ZEXPECT(settings.cache_dir_defaulted == true);
}

ZEST_CASE(FinalizeEmptyWorkspace) {
    Settings settings;
    settings.finalize(CanonicalPath());
    ZEXPECT(settings.cache_dir.empty());
    ZEXPECT(settings.logging_dir.empty());
}

ZEST_CASE(FinalizePreservesSet) {
    Settings settings;
    settings.cache_dir = "/custom";
    settings.poll_seconds = 5;
    settings.inlay_hints.parameters = false;
    settings.inlay_hints.block_end = true;
    settings.finalize(CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(std::string_view(settings.cache_dir) ==
            CanonicalPath(Spelling::absolute("/custom")).str());
    ZEXPECT(settings.cache_dir_defaulted == false);
    ZEXPECT(settings.poll_seconds == 5u);
    ZEXPECT(settings.inlay_hints.parameters == false);
    ZEXPECT(settings.inlay_hints.block_end == true);
}

ZEST_CASE(OverlayWithoutFile) {
    TempDir tmp;
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "cache_dir": "/opt/cache",
        "workers": { "foreground": 3 },
        "rules": [
            { "patterns": ["**/*.cpp"], "append": ["-DFOO"] }
        ]
    })")});
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(std::string_view(loaded.settings.cache_dir) ==
            CanonicalPath(Spelling("/opt/cache", Spelling::absolute(tmp.root))).str());
    ZEXPECT(loaded.settings.workers.foreground == 3u);
    ZEXPECT(loaded.settings.rules.size() == 1u);
    ZEXPECT(loaded.settings.compiled_rules.size() == 1u);
}

ZEST_CASE(LoadMalformedTOML) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers\nbroken");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.file == path::join(root, "clice.toml"));
    ZEXPECT(loaded.settings.workers.foreground == 2u);
}

ZEST_CASE(MissingFileUsesDefaults) {
    TempDir tmp;
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.file.empty());
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(loaded.settings.workers.foreground == 2u);
}

ZEST_CASE(WorkspaceVarSubst) {
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    Settings settings;
    settings.cache_dir = "${workspace}/cache";
    settings.logging_dir = "${workspace}/logs";
    settings.rules.push_back(Rule{.compile_commands = {"${workspace}/build"}});
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(std::string_view(settings.cache_dir) == at("cache"));
    ZEXPECT(std::string_view(settings.logging_dir) == at("logs"));
    ZASSERT(settings.compiled_rules.size() == 1u);
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at("build"));
}

ZEST_CASE(EnvVariableSubstituted) {
    // `${env:NAME}` substitutes in the directories and the rules; a string
    // command is split before, so a value with spaces stays one argument.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    ScopedEnv directory("CLICE_TEST_SETTINGS_DIR", tmp.path("my dir"));
    Settings settings;
    settings.cache_dir = "${env:CLICE_TEST_SETTINGS_DIR}/cache";
    settings.rules.push_back(Rule{
        .compile_commands = {"${env:CLICE_TEST_SETTINGS_DIR}/build"},
        .default_command = std::string("clang++ -I${env:CLICE_TEST_SETTINGS_DIR}/include -c"),
    });
    ZEXPECT(settings.finalize(CanonicalPath(Spelling::absolute(tmp.root))).empty());
    ZEXPECT(std::string_view(settings.cache_dir) == at("my dir/cache"));
    ZASSERT(settings.compiled_rules.size() == 1u);
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at("my dir/build"));
    std::vector<std::string> command = {"clang++", "-I" + tmp.path("my dir") + "/include", "-c"};
    ZEXPECT(settings.compiled_rules[0].default_command == command);
}

ZEST_CASE(UnsetVariableRejected) {
    // An unset variable is an error at its key, naming it; the value is
    // left out.
    TempDir tmp;
    ScopedEnv unset("CLICE_TEST_UNSET_VARIABLE", std::nullopt);
    tmp.touch("clice.toml", R"([[rules]]
compile_commands = ["${env:CLICE_TEST_UNSET_VARIABLE}/build"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("CLICE_TEST_UNSET_VARIABLE") != std::string::npos);
    ZEXPECT(issue.line == 2u);
    ZEXPECT(issue.column == 21u);
    ZASSERT(loaded.settings.compiled_rules.size() == 1u);
    ZEXPECT(loaded.settings.compiled_rules[0].compile_commands.empty());
}

ZEST_CASE(UnknownVariableRejected) {
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{.append = {"-I${foo}/include"}});
    auto problems = settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZASSERT(problems.size() == 1u);
    ZEXPECT(llvm::StringRef(problems[0].message).contains("${foo}"));
    ZEXPECT((problems[0].path ==
             std::vector<codec::rich_error::path_segment>{"rules", std::size_t(0), "append"}));
    ZASSERT(settings.compiled_rules.size() == 1u);
    ZEXPECT(settings.compiled_rules[0].append.empty());
}

ZEST_CASE(EnvValueMatchesLiterally) {
    // A value substituted into a pattern is a path: its glob characters
    // match themselves.
    TempDir tmp;
    tmp.touch("lib[1]/x.cpp", "");
    tmp.touch("lib1/x.cpp", "");
    ScopedEnv library("CLICE_TEST_LIBRARY_DIR", tmp.path("lib[1]"));
    Settings settings;
    settings.rules.push_back(
        Rule{.patterns = {"${env:CLICE_TEST_LIBRARY_DIR}/**"}, .append = {"-DLIBRARY"}});
    CanonicalPath root(Spelling::absolute(tmp.root));
    ZEXPECT(settings.finalize(root).empty());
    Rules rules(settings, root, "");
    ZEXPECT(rules.matching(CanonicalPath(Spelling::absolute(tmp.path("lib[1]/x.cpp")))).size() ==
            1u);
    ZEXPECT(rules.matching(CanonicalPath(Spelling::absolute(tmp.path("lib1/x.cpp")))).empty());
}

ZEST_CASE(BackslashPatternRejected) {
    // A backslash escapes in a glob: a pattern written with one is an error
    // at it, the rule's other patterns still apply.
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
patterns = ['src\*.cpp', "lib/**"]
append = ["-DX"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.message.find("backslash") != std::string::npos);
    ZEXPECT(issue.line == 2u);
    ZEXPECT(issue.column == 13u);
    ZASSERT(loaded.settings.compiled_rules.size() == 1u);
    ZEXPECT(loaded.settings.compiled_rules[0].patterns.size() == 1u);
    ZEXPECT(!loaded.settings.compiled_rules[0].unmatchable);
}

ZEST_CASE(ParseRuleSources) {
    auto result = codec::toml::from_string<Settings>(R"(
default_configuration = "debug"

[[rules]]
configuration = "debug"
compile_commands = ["build/debug", "/abs/compile_commands.json"]
default_command = "clang++ -std=c++20 -Iinclude"

[[rules]]
patterns = ["third_party/**"]
default_command = ["clang", "-std=c17"]
background_index = false
)");
    ZASSERT(result);
    ZEXPECT(std::string_view(result->default_configuration) == "debug");
    ZASSERT(result->rules.size() == 2u);
    ZEXPECT(std::string_view(result->rules[0].configuration) == "debug");
    ZEXPECT(result->rules[0].compile_commands.size() == 2u);
    ZEXPECT(std::get<std::string>(result->rules[0].default_command) ==
            "clang++ -std=c++20 -Iinclude");
    ZEXPECT(!result->rules[0].background_index.has_value());
    ZEXPECT(std::get<std::vector<std::string>>(result->rules[1].default_command).size() == 2u);
    ZEXPECT(result->rules[1].background_index == false);
}

ZEST_CASE(AnchoredRules) {
    /// Every rule anchors at the project root, also one written in
    /// .clice/config.toml: relative databases and patterns; `**`-led and
    /// absolute patterns match absolute paths; the databases of a rule
    /// without patterns are the project's.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    auto identity = [&](llvm::StringRef relative) {
        return CanonicalPath(Spelling::absolute(at(relative)));
    };
    std::string root = tmp.root;
    path::canonicalize(root);
    tmp.touch(".clice/config.toml",
              std::format(R"(
[[rules]]
patterns = ["src/**"]
configuration = "debug"
compile_commands = ["out/debug"]
default_command = "clang++ -std=c++20 -I${{workspace}}/include"

[[rules]]
patterns = ["**/*.hxx", "${{workspace}}/gen/**", "shared/*.cpp", "*"]
append = ["-x", "c++-header"]

[[rules]]
patterns = ["release/**"]
configuration = "release"

[[rules]]
compile_commands = ["build", "{}"]
)",
                          at("elsewhere/compile_commands.json")));

    auto loaded = load_settings(CanonicalPath(Spelling::absolute(root)));
    ZASSERT(!loaded.broken());
    auto& settings = loaded.settings;

    ZASSERT(settings.compiled_rules.size() == 4u);
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at("out/debug"));
    ZEXPECT(settings.compiled_rules[0].patterns[0].root == identity("src"));
    ZEXPECT(settings.compiled_rules[0].declares_sources());
    /// The string spelling is tokenized, then `${workspace}` substituted
    /// per argument.
    ZASSERT(settings.compiled_rules[0].default_command.size() == 3u);
    ZEXPECT(settings.compiled_rules[0].default_command[2] == "-I" + at("include"));
    ZEXPECT(!settings.compiled_rules[1].declares_sources());
    ZASSERT(settings.compiled_rules[1].patterns.size() == 4u);
    ZEXPECT(settings.compiled_rules[1].patterns[0].root == CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(settings.compiled_rules[1].patterns[1].root == identity("gen"));
    ZEXPECT(settings.compiled_rules[1].patterns[2].root == identity("shared"));
    ZEXPECT(settings.compiled_rules[1].patterns[3].root == CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(settings.compiled_rules[3].patterns.empty());
    ZEXPECT(settings.compiled_rules[3].compile_commands[0].str() == at("build"));
    ZEXPECT(settings.compiled_rules[3].compile_commands[1].str() ==
            at("elsewhere/compile_commands.json"));

    auto tags = settings.configurations();
    ZASSERT(tags.size() == 2u);
    ZEXPECT(tags[0] == "debug");
    ZEXPECT(tags[1] == "release");

    /// A relative pattern sees only files under the root; a bare `*` names
    /// the root's direct children.
    CanonicalPath project_root(Spelling::absolute(root));
    Rules debug(settings, project_root, "debug");
    Rules release(settings, project_root, "release");
    ZEXPECT(debug.matching(identity("src/a.cpp")).size() == 2u);
    ZEXPECT(release.matching(identity("src/a.cpp")).size() == 1u);
    ZEXPECT(debug.matching(identity(".clice/src/a.cpp")).size() == 1u);
    ZEXPECT(debug.matching(identity("a.cpp")).size() == 2u);
    /// `**` and `${workspace}` patterns match the absolute path, `*` stays
    /// within one segment.
    auto hxx = debug.matching(identity("other/tree/x.hxx"));
    ZASSERT(hxx.size() == 2u);
    ZEXPECT(hxx[0]->append.size() == 2u);
    ZEXPECT(debug.matching(identity("gen/x.cpp")).size() == 2u);
    ZEXPECT(debug.matching(identity("shared/x.cpp")).size() == 2u);
    ZEXPECT(debug.matching(identity("shared/deep/x.cpp")).size() == 1u);
}

ZEST_CASE(OverlayAnchorsAtRoot) {
    /// Rules overlaid through initializationOptions anchor at the project
    /// root too.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    std::string root = tmp.root;
    path::canonicalize(root);
    auto overlaid = load_settings(CanonicalPath(Spelling::absolute(root)), {.overlay = overlay(R"({
        "rules": [
            { "patterns": ["src/**"], "compile_commands": ["cmake"] },
            { "compile_commands": ["out"] }
        ]
    })")});
    auto& settings = overlaid.settings;

    ZASSERT(settings.compiled_rules.size() == 2u);
    ZEXPECT(settings.compiled_rules[0].patterns[0].root ==
            CanonicalPath(Spelling::absolute(at("src"))));
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at("cmake"));
    ZEXPECT(settings.compiled_rules[1].compile_commands[0].str() == at("out"));
}

ZEST_CASE(RuleOrigins) {
    /// A rule names where it is written: its file and the line it starts
    /// at, or its place in the editor's settings.
    TempDir tmp;
    tmp.touch("clice.toml", R"(cache_dir = "cache"

[[rules]]
append = ["-DA"]

[[rules]]
patterns = ["src/**"]
append = ["-DB"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto file = path::join(root, "clice.toml");
    auto from_file = load_settings(root).settings;
    ZASSERT(from_file.compiled_rules.size() == 2u);
    ZEXPECT(from_file.compiled_rules[0].origin == file + ":3");
    ZEXPECT(from_file.compiled_rules[1].origin == file + ":6");

    auto overlaid =
        load_settings(root, {.overlay = overlay(R"({ "rules": [{ "append": ["-DC"] }] })")});
    ZASSERT(overlaid.settings.compiled_rules.size() == 1u);
    ZEXPECT(overlaid.settings.compiled_rules[0].origin == "initializationOptions rules[0]");
}

ZEST_CASE(RelativeProjectDirsAnchor) {
    /// A relative cache or log directory anchors where every other relative
    /// path does: at the project root, whatever file or overlay it came
    /// from.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    std::string root = tmp.root;
    path::canonicalize(root);
    tmp.touch(".clice/config.toml", "cache_dir = \"cache\"\n");

    auto loaded = load_settings(CanonicalPath(Spelling::absolute(root)));
    ZASSERT(!loaded.broken());
    ZEXPECT(std::string_view(loaded.settings.cache_dir) == at("cache"));
    ZEXPECT(std::string_view(loaded.settings.logging_dir) == at("cache/logs"));

    Settings settings;
    settings.logging_dir = "logs";
    settings.finalize(CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(std::string_view(settings.logging_dir) == at("logs"));
}

ZEST_CASE(SourcesOffByDefault) {
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"**/*"}, .append = {"-DX"}});
    settings.finalize(CanonicalPath(Spelling::absolute("/ws")));
    ZEXPECT(!settings.compiled_rules[0].declares_sources());
    ZEXPECT(settings.configurations().empty());
}

ZEST_CASE(InvalidGlobPattern) {
    Settings settings;
    // All-invalid patterns: the rule matches nothing (its flags never
    // apply), but the database it declares still loads.
    settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}"},
        .compile_commands = {"/elsewhere/compile_commands.json"},
        .append = {"-DSHOULD_NOT_APPEAR"},
    });
    // Mixed valid/invalid: only the invalid pattern is skipped; rule remains.
    settings.rules.push_back(Rule{
        .patterns = {"**/****.{c,cc}", "**/*.cpp"},
        .append = {"-DCPP"},
    });
    settings.finalize(CanonicalPath(Spelling::absolute("/src")));
    ZASSERT(settings.compiled_rules.size() == 2u);
    ZEXPECT(settings.compiled_rules[0].unmatchable);
    ZEXPECT(settings.compiled_rules[0].declares_sources());
    ZEXPECT(!settings.compiled_rules[1].unmatchable);

    std::vector<std::string> append, remove;
    match_rules(settings, "/src/foo.cpp", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-DCPP");
}

ZEST_CASE(OverlayPriority) {
    // initializationOptions-sourced config should override an on-disk default.
    TempDir tmp;
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)),
                      {.overlay = overlay(R"({ "workers": { "background_delay_ms": 42 } })")});
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(loaded.settings.workers.background_delay_ms == 42u);
    // Unset fields still receive defaults.
    ZEXPECT(loaded.settings.workers.foreground == 2u);
    ZEXPECT(loaded.settings.poll_seconds == 30u);
}

ZEST_CASE(DefaultWorkspaceCache) {
    Settings settings;
    CanonicalPath root(Spelling::absolute("/ws/root"));
    settings.finalize(root);

    ZEXPECT(std::string_view(settings.cache_dir) == root.str() + "/.clice");
    ZEXPECT(std::string_view(settings.logging_dir) == root.str() + "/.clice/logs");
}

ZEST_CASE(WorkspaceSubstEmpty) {
    // Empty project_root must not rewrite "${workspace}" into "" and produce
    // bogus paths like "/cache": the directory stays relative, and a project
    // without a folder has nothing to anchor it at, so it is dropped.
    Settings settings;
    settings.cache_dir = "${workspace}/cache";
    settings.finalize(CanonicalPath());
    ZEXPECT(settings.cache_dir.empty());
}

ZEST_CASE(RootlessRulesIgnored) {
    // A server without folders has nothing to anchor a rule's paths at:
    // the rule is dropped instead of matching against the process's cwd.
    Settings settings;
    settings.rules.push_back(Rule{.patterns = {"src/**"}, .append = {"-DX"}});
    settings.finalize(CanonicalPath());
    ZEXPECT(settings.compiled_rules.empty());
}

ZEST_CASE(HomeExpanded) {
    // A leading `~` is the home directory, not a directory named `~`.
    TempDir tmp;
    Settings settings;
    settings.cache_dir = "~/clice-cache";
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    std::string home;
    ZASSERT(!sys::home_dir(home));
    ZEXPECT(std::string_view(settings.cache_dir) ==
            CanonicalPath(Spelling("clice-cache", Spelling::absolute(home))).str());
}

ZEST_CASE(WorkspaceSubstRepeated) {
    // Multiple ${workspace} occurrences in one string all get substituted.
    Settings settings;
    settings.cache_dir = "${workspace}/a/${workspace}/b";
    CanonicalPath root(Spelling::absolute("/root"));
    settings.finalize(root);
    ZEXPECT(std::string_view(settings.cache_dir) ==
            CanonicalPath(Spelling::absolute(root.str() + "/a/" + root.str() + "/b")).str());
}

ZEST_CASE(CompileCommandsList) {
    // Every top-level database path substitutes ${workspace} and anchors
    // at the project root; absolute ones pass through, and an
    // existing directory names the database under it whatever its suffix.
    TempDir tmp;
    tmp.mkdir("build.json");
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    Settings settings;
    settings.rules.push_back(Rule{
        .compile_commands = {
                             "${workspace}/build", at("abs/path/compile_commands.json"),
                             "out", "build.json",
                             }
    });
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZASSERT(settings.compiled_rules.size() == 1u);
    auto& databases = settings.compiled_rules[0].compile_commands;
    ZASSERT(databases.size() == 4u);
    ZEXPECT(databases[0].str() == at("build"));
    ZEXPECT(databases[1].str() == at("abs/path/compile_commands.json"));
    ZEXPECT(databases[2].str() == at("out"));
    ZEXPECT(databases[3].str() == at("build.json/compile_commands.json"));
}

ZEST_CASE(TOMLErrorLocated) {
    // A syntax error sits where the parser stopped: the line break that
    // ends the table header before its `]`.
    TempDir tmp;
    tmp.touch("clice.toml", "[workers\nforeground = 3\n");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.line == 1u);
    ZEXPECT(issue.column == 9u);
}

ZEST_CASE(SyntaxIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers\nforeground = 3\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
}

ZEST_CASE(TypeIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers]\nforeground = \"yes\"\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(loaded.issues[0].message.find("foreground") != std::string::npos);
}

ZEST_CASE(UnknownKeyWarns) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers]\nmemory_limit = 4294967296\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Warning);
    ZEXPECT(loaded.issues[0].message.find("memory_limit") != std::string::npos);
}

ZEST_CASE(MovedKeysRejected) {
    /// A key clice 1.0 moved is an error naming its new place, every key of
    /// a removed section, a rule's `index` too; another unknown key of a
    /// rule stays a warning. What the moved keys say does not apply.
    TempDir tmp;
    tmp.touch("clice.toml", R"([project]
cache_dir = "cache"
enable_indexing = false
worker_memory_limit = 4294967296

[tracker]
workspace_poll_seconds = 5

[[rules]]
patterns = ["vendor/**"]
index = false
apend = ["-DX"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.issues.size() == 6u);
    auto issue_at = [&](std::uint32_t line) -> const SettingsIssue& {
        auto found = ranges::find(loaded.issues, line, &SettingsIssue::line);
        ZASSERT(found != loaded.issues.end());
        return *found;
    };
    auto says = [](const SettingsIssue& issue, llvm::StringRef text) {
        return llvm::StringRef(issue.message).contains(text);
    };
    ZEXPECT(says(issue_at(2), "moved to the top-level `cache_dir` in clice 1.0 at project.cache_dir"));
    ZEXPECT(says(issue_at(3), "a last pattern-less rule's `background_index = false`"));
    ZEXPECT(says(issue_at(4), "gone in clice 1.0 at project.worker_memory_limit"));
    ZEXPECT(says(issue_at(7), "moved to the top-level `poll_seconds` in clice 1.0 at tracker.workspace_poll_seconds"));
    ZEXPECT(says(issue_at(11), "moved to `background_index` in clice 1.0 at rules[0].index"));
    for(auto line: {2u, 3u, 4u, 7u, 11u}) {
        ZEXPECT(issue_at(line).severity == SettingsIssue::Severity::Error);
        ZEXPECT(issue_at(line).file == path::join(root, "clice.toml"));
    }
    ZEXPECT(says(issue_at(12), "apend"));
    ZEXPECT(issue_at(12).severity == SettingsIssue::Severity::Warning);

    ZEXPECT(loaded.settings.cache_dir_defaulted);
    ZEXPECT(loaded.settings.poll_seconds == 30u);
    ZASSERT(loaded.settings.compiled_rules.size() == 1u);
    ZEXPECT(!loaded.settings.compiled_rules[0].background_index.has_value());
}

ZEST_CASE(OverlayMovedKeysRejected) {
    TempDir tmp;
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "project": { "stateful_worker_count": 3, "test_hooks": true },
        "rules": [{ "patterns": ["vendor/**"], "index": false }],
        "inlay_hints": { "blokc_end": true }
    })")});
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.issues.size() == 4u);
    auto issue_saying = [&](llvm::StringRef text) -> const SettingsIssue& {
        auto found = ranges::find_if(loaded.issues, [&](const SettingsIssue& issue) {
            return llvm::StringRef(issue.message).contains(text);
        });
        ZASSERT(found != loaded.issues.end());
        return *found;
    };
    for(auto text: {"moved to `workers.foreground` in clice 1.0 at project.stateful_worker_count",
                    "moved to the environment variable CLICE_TEST_HOOKS in clice 1.0 at project.test_hooks",
                    "moved to `background_index` in clice 1.0 at rules[0].index"}) {
        auto& issue = issue_saying(text);
        ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
        ZEXPECT(issue.file.empty());
        ZEXPECT(issue.message.starts_with("initializationOptions: "));
    }
    ZEXPECT(issue_saying("blokc_end").severity == SettingsIssue::Severity::Warning);
    ZEXPECT(loaded.settings.workers.foreground == 2u);
}

ZEST_CASE(UnknownFeatureKeyWarns) {
    // Feature options structs double as config sections; a typo inside one
    // must still be reported as a Warning, not vanish.
    TempDir tmp;
    tmp.touch("clice.toml", "[inlay_hints]\nblokc_end = true\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Warning);
    ZEXPECT(loaded.issues[0].message.find("blokc_end") != std::string::npos);
}

ZEST_CASE(CapitalizedReadonlyRejected) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
readonly = "On"
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("readonly") != std::string::npos);
    ZEXPECT(issue.line == 2u);
    ZEXPECT(issue.column == 12u);
    ZEXPECT(loaded.settings.rules.empty());
}

ZEST_CASE(InvalidGlobLocated) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
patterns = ["src/[a"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("src/[a") != std::string::npos);
    ZEXPECT(issue.line == 2u);
    ZEXPECT(issue.column == 13u);
}

ZEST_CASE(DefaultNamesNoTag) {
    TempDir tmp;
    tmp.touch("clice.toml", R"(default_configuration = "relase"

[[rules]]
configuration = "debug"
append = ["-DDEBUG"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("relase") != std::string::npos);
    ZEXPECT(issue.line == 1u);
    ZEXPECT(issue.column == 25u);
}

ZEST_CASE(CountsAndLimitsDecode) {
    // A count is "auto" or a number, a limit "unlimited" or a number.
    TempDir tmp;
    tmp.touch("clice.toml", R"([workers]
background = 6
max = "auto"

[references]
limit = "unlimited"

[code_completion]
limit = 7

[inlay_hints.max_length]
types = "unlimited"
)");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.issues.empty());
    auto& settings = loaded.settings;
    ZASSERT(std::holds_alternative<std::uint32_t>(settings.workers.background));
    ZEXPECT(std::get<std::uint32_t>(settings.workers.background) == 6u);
    ZEXPECT(settings.workers.background_count() == 6u);
    ZEXPECT(std::holds_alternative<Automatic>(settings.workers.max));
    ZEXPECT(settings.workers.max_count() == default_max_stateless_worker_count());
    ZEXPECT(std::holds_alternative<NoLimit>(settings.references.limit));
    ZEXPECT(at_most(settings.references.limit) == std::numeric_limits<std::uint32_t>::max());
    ZEXPECT(at_most(settings.code_completion.limit) == 7u);
    ZEXPECT(std::holds_alternative<NoLimit>(settings.inlay_hints.max_length.types));
    ZEXPECT(at_most(settings.inlay_hints.max_length.block_end) == 60u);

    auto overlaid =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "workers": { "background": "auto", "max": 3 },
        "references": { "limit": 50 }
    })")});
    ZEXPECT(overlaid.issues.empty());
    ZEXPECT(std::holds_alternative<Automatic>(overlaid.settings.workers.background));
    ZEXPECT(overlaid.settings.workers.max_count() == 3u);
    ZEXPECT(at_most(overlaid.settings.references.limit) == 50u);
}

ZEST_CASE(ZeroCountsRejected) {
    // An explicit 0 is not a runnable count or limit: finalize reports it
    // at its key and, meanwhile, falls back to the born-valid default
    // instead of starting a pool with no workers or answering nothing.
    Settings settings;
    settings.workers.foreground = 0;
    settings.workers.background = 0u;
    settings.workers.max = 0u;
    settings.references.limit = 0u;
    settings.code_completion.limit = 0u;
    settings.inlay_hints.max_length.endif = 0u;
    auto problems = settings.finalize(CanonicalPath());
    ZASSERT(problems.size() == 6u);
    ZEXPECT(problems[0].to_string() == "a worker count must be at least 1 at workers.foreground");
    ZEXPECT(problems[1].to_string() ==
            R"(a worker count must be at least 1, or "auto" at workers.background)");
    ZEXPECT(problems[2].to_string() ==
            R"(a worker count must be at least 1, or "auto" at workers.max)");
    ZEXPECT(problems[3].to_string() ==
            R"(a limit must be at least 1; "unlimited" lifts it at references.limit)");
    ZEXPECT(problems[4].to_string() ==
            R"(a limit must be at least 1; "unlimited" lifts it at code_completion.limit)");
    ZEXPECT(problems[5].to_string() ==
            R"(a limit must be at least 1; "unlimited" lifts it at inlay_hints.max_length.endif)");
    ZEXPECT(settings.workers.foreground == 2u);
    ZEXPECT(std::holds_alternative<Automatic>(settings.workers.background));
    ZEXPECT(std::holds_alternative<Automatic>(settings.workers.max));
    ZEXPECT(at_most(settings.references.limit) == 2000u);
    ZEXPECT(at_most(settings.code_completion.limit) == 100u);
    ZEXPECT(at_most(settings.inlay_hints.max_length.endif) == 60u);
}

ZEST_CASE(ZeroCountLocated) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([workers]
foreground = 0

[references]
limit = 0
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 2u);
    auto& count = loaded.issues[0];
    ZEXPECT(count.severity == SettingsIssue::Severity::Error);
    ZEXPECT(count.file == path::join(root, "clice.toml"));
    ZEXPECT(count.message.find("foreground") != std::string::npos);
    ZEXPECT(count.line == 2u);
    ZEXPECT(count.column == 14u);
    auto& limit = loaded.issues[1];
    ZEXPECT(limit.message.find("references.limit") != std::string::npos);
    ZEXPECT(limit.line == 5u);
    ZEXPECT(limit.column == 9u);
    ZEXPECT(loaded.settings.workers.foreground == 2u);
    ZEXPECT(at_most(loaded.settings.references.limit) == 2000u);
}

ZEST_CASE(StrictRejectsUnknownConfiguration) {
    TempDir tmp;
    tmp.touch("clice.toml", R"(
default_configuration = "debug"

[[rules]]
configuration = "debug"
append = ["-DDEBUG"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));

    auto strict = load_settings(root, {.configuration = "nope"});
    ZEXPECT(strict.broken());
    ZASSERT(strict.issues.size() == 1u);
    ZEXPECT(strict.issues[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(strict.issues[0].file.empty());
    ZEXPECT(strict.issues[0].message.find("nope") != std::string::npos);
    ZEXPECT(strict.configuration == "debug");

    // Served, the name is skipped with a warning.
    auto lax = load_settings(root, {.configuration = "nope", .strict = false});
    ZEXPECT(!lax.broken());
    ZASSERT(lax.issues.size() == 1u);
    ZEXPECT(lax.issues[0].severity == SettingsIssue::Severity::Warning);
    ZEXPECT(lax.configuration == "debug");
}

ZEST_CASE(NullOptionRejected) {
    // `defaulted` means "may be absent", never "may be null": an explicit
    // JSON null on an option is a type error and fails the whole decode,
    // both flat and inside a feature section.
    TempDir tmp;
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto flat =
        load_settings(root, {.overlay = overlay(R"({ "workers": { "foreground": null } })")});
    ZEXPECT(flat.broken());
    auto nested =
        load_settings(root, {.overlay = overlay(R"({ "inlay_hints": { "block_end": null } })")});
    ZEXPECT(nested.broken());
}

ZEST_CASE(OverlayUnknownKeyWarns) {
    TempDir tmp;
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "workers": { "background_dealy_ms": 5, "background_delay_ms": 42 }
    })")});
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Warning);
    ZEXPECT(issue.file.empty());
    ZEXPECT(issue.message.starts_with("initializationOptions: "));
    ZEXPECT(issue.message.find("background_dealy_ms") != std::string::npos);
    ZEXPECT(loaded.settings.workers.background_delay_ms == 42u);
}

ZEST_CASE(BadOverlayAppliesNothing) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers]\nbackground_delay_ms = 16\n");
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "rules": [{ "patterns": ["**/*.cpp"], "append": ["-DOVERLAY"] }],
        "workers": { "background_delay_ms": 99, "foreground": "yes" }
    })")});
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file.empty());
    ZEXPECT(issue.message.starts_with("initializationOptions: "));
    ZEXPECT(issue.message.find("workers.foreground") != std::string::npos);
    ZEXPECT(loaded.settings.rules.empty());
    ZEXPECT(loaded.settings.workers.background_delay_ms == 16u);
    ZEXPECT(loaded.settings.workers.foreground == 2u);
}

ZEST_CASE(MalformedTomlNoFallback) {
    TempDir tmp;
    tmp.touch("clice.toml", "[workers\ninvalid");
    tmp.touch(".clice/config.toml", "[workers]\nforeground = 5\n");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.file == path::join(root, "clice.toml"));
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(loaded.issues[0].file == path::join(root, "clice.toml"));
    ZEXPECT(loaded.settings.workers.foreground == 2u);
}

ZEST_CASE(ShadowedFieldWarns) {
    // A rule for every file that is not last decides each field it sets
    // before any rule after it: a later rule setting one is told so, at the
    // field. A tagged rule for every file does not decide for an untagged
    // one, active when it is not.
    TempDir tmp;
    tmp.touch("clice.toml", R"([[rules]]
lint = false

[[rules]]
patterns = ["src/**"]
lint = true
append = ["-DX"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Warning);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("rules[0]") != std::string::npos);
    ZEXPECT(issue.line == 6u);
    ZEXPECT(issue.column == 8u);

    tmp.touch("clice.toml", R"([[rules]]
configuration = "debug"
lint = false

[[rules]]
patterns = ["src/**"]
lint = true
)");
    ZEXPECT(load_settings(root).issues.empty());
}

ZEST_CASE(LaterRemoveWins) {
    // Later rule's `remove` must cancel an earlier rule's matching `append`.
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO", "-DBAR"},
    });
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .remove = {"-DFOO"},
    });
    CanonicalPath project_root(Spelling::absolute("/src"));
    settings.finalize(project_root);

    // The edits stay in rule order; the remove takes effect against the
    // earlier append when the command is built, and against the base.
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    cdb.add_command("/src", "/src/a.cpp", std::string_view("clang++ -DFOO a.cpp"));
    Rules rules(settings, project_root, "");
    auto edits = rules.edits({CanonicalPath(Spelling::absolute("/src/a.cpp"))});
    ZASSERT(edits.edits.size() == 2u);
    ZEXPECT(edits.edits[1].kind == CommandEdit::Kind::Remove);
    ZEXPECT(join_command(render_entry(cdb, "/src/a.cpp", edits.options())) ==
            "clang++ -D BAR " + CanonicalPath(Spelling::absolute("/src/a.cpp")).str());
}

ZEST_CASE(LaterAppendWins) {
    // Later append comes after earlier append — at compiler level, last wins
    // for flags like -O; verify the ordering is preserved.
    Settings settings;
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-O2"},
    });
    settings.rules.push_back(Rule{
        .patterns = {"**/*.cpp"},
        .append = {"-O3"},
    });
    settings.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(settings, "/src/a.cpp", append, remove);
    ZEXPECT(append.size() == 2u);
    ZEXPECT(append[0] == "-O2");
    ZEXPECT(append[1] == "-O3");
}

ZEST_CASE(OverlayPreservesTOML) {
    // Mirror the master_server flow: load the project settings from clice.toml first,
    // then overlay initializationOptions JSON. Fields absent in the JSON must
    // keep their clice.toml values; fields present in the JSON override.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
cache_dir = "/from/toml"
poll_seconds = 7

[workers]
background_delay_ms = 16

[[rules]]
patterns = ["**/*.cpp"]
append = ["-DFROM_TOML"]
)");

    CanonicalPath root(Spelling::absolute(tmp.root));
    auto from_file = load_settings(root).settings;
    auto from_toml = CanonicalPath(Spelling("/from/toml", Spelling::absolute(tmp.root))).str();
    ZEXPECT(std::string_view(from_file.cache_dir) == from_toml);
    ZEXPECT(from_file.poll_seconds == 7u);
    ZEXPECT(from_file.workers.background_delay_ms == 16u);
    ZEXPECT(from_file.compiled_rules.size() == 1u);

    // Overlay only `background_delay_ms` via JSON.
    auto overlaid =
        load_settings(root,
                      {.overlay = overlay(R"({ "workers": { "background_delay_ms": 99 } })")});
    ZEXPECT(overlaid.issues.empty());
    auto& settings = overlaid.settings;

    // Overridden field.
    ZEXPECT(settings.workers.background_delay_ms == 99u);
    // Untouched fields stay at TOML values.
    ZEXPECT(std::string_view(settings.cache_dir) == from_toml);
    ZEXPECT(settings.poll_seconds == 7u);
    // Rules from clice.toml must survive the overlay.
    ZEXPECT(settings.rules.size() == 1u);
    ZEXPECT(settings.compiled_rules.size() == 1u);
    ZEXPECT(settings.rules[0].append[0] == "-DFROM_TOML");
}

ZEST_CASE(OverlaySectionDeepMerge) {
    // The load-bearing layering semantic: overlaying a JSON source that
    // names one field of a section must merge into the section in place —
    // fields the TOML layer set survive, fields nobody named keep their
    // defaults. If a decode ever rebuilt the section object wholesale,
    // this pins the regression.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[inlay_hints]
block_end = true

[code_completion]
bundle_overloads = false
)");
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "inlay_hints": { "parameters": false, "block_end": false },
        "code_completion": { "limit": 5 }
    })")});
    ZEXPECT(loaded.issues.empty());
    auto& settings = loaded.settings;

    // From the TOML layer.
    ZEXPECT(settings.code_completion.bundle_overloads == false);
    // From the JSON overlay, including a nested field both layers set —
    // the later source wins.
    ZEXPECT(settings.inlay_hints.block_end == false);
    ZEXPECT(settings.inlay_hints.parameters == false);
    ZEXPECT(at_most(settings.code_completion.limit) == 5u);
    // Named by nobody: field-initializer defaults.
    ZEXPECT(settings.inlay_hints.deduced_types == true);
    ZEXPECT(settings.code_completion.arguments == feature::CompletionArguments::Placeholders);
}

ZEST_CASE(OverlayRulesReplace) {
    // When `rules` is present in the overlay JSON, it replaces the whole array
    // (kotatsu deserializes the vector by value). `compiled_rules` must be
    // rebuilt after finalize so stale compiled entries don't linger.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[[rules]]
patterns = ["**/*.cpp"]
append = ["-DTOML_ONLY"]
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto from_file = load_settings(root).settings;
    ZEXPECT(from_file.compiled_rules.size() == 1u);

    auto loaded = load_settings(root, {.overlay = overlay(R"({
        "rules": [{ "patterns": ["**/*.cc"], "append": ["-DFROM_JSON"] }]
    })")});
    ZEXPECT(loaded.issues.empty());
    auto& settings = loaded.settings;

    ZEXPECT(settings.rules.size() == 1u);
    ZEXPECT(settings.rules[0].append[0] == "-DFROM_JSON");
    ZEXPECT(settings.compiled_rules.size() == 1u);

    // Original TOML rule no longer applies.
    std::vector<std::string> append, remove;
    match_rules(settings, "/src/x.cpp", append, remove);
    ZEXPECT(append.empty());
    match_rules(settings, "/src/x.cc", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-DFROM_JSON");
}

ZEST_CASE(JsonSchema) {
    auto schema = Settings::json_schema();
    ZASSERT(schema);
    auto doc = json::from_string<codec::dyn::Value>(*schema);
    ZASSERT(doc);

    // The worker counts the machine decides default to "auto", never to the
    // machine's number, so the schema stays byte-identical across hosts.
    for(auto field: {"background", "max"}) {
        const auto* count = find_property(*doc, field);
        ZASSERT(count != nullptr);
        const auto* object = count->get_object();
        ZASSERT(object != nullptr);
        ZEXPECT(object->find("description") != nullptr);
        const auto* fallback = object->find("default");
        ZASSERT(fallback != nullptr);
        ZEXPECT(*fallback == codec::dyn::Value("auto"));
    }
    // A stable field appears under the section default.
    ZEXPECT(default_mentions(*doc, "background_delay_ms", false));

    // A stable field initializer survives as the schema default, and the
    // unsigned field type keeps negative delays out of the schema.
    const auto* delay = find_property(*doc, "background_delay_ms");
    ZASSERT(delay != nullptr);
    ZEXPECT(delay->get_object()->find("default") != nullptr);
    const auto* delay_minimum = delay->get_object()->find("minimum");
    ZASSERT(delay_minimum != nullptr);
    ZEXPECT(delay_minimum->get_uint().value_or(1) == 0u);

    // skip = true fields stay out of the schema entirely.
    ZEXPECT(find_property(*doc, "compiled_rules") == nullptr);

    // A field finalize() rejects `0` for carries the matching lower bound.
    const auto* foreground = find_property(*doc, "foreground");
    ZASSERT(foreground != nullptr);
    const auto* minimum = foreground->get_object()->find("minimum");
    ZASSERT(minimum != nullptr);
    ZEXPECT(minimum->get_uint().value_or(0) == 1u);

    // Enum fields name their accepted values, so editors flag a typo that
    // the lenient decode would silently turn into the default; a rule's,
    // which it may leave out, beside null.
    const auto* readonly = find_property(*doc, "readonly");
    ZASSERT(readonly != nullptr);
    const auto* branches = readonly->get_object()->find("anyOf");
    ZASSERT(branches != nullptr);
    const auto* modes = (*branches->get_array())[0].get_object()->find("enum");
    ZASSERT(modes != nullptr);
    ZEXPECT(*modes == codec::dyn::Value(codec::dyn::Array{"off", "on", "auto"}));

    // Root and every section body reject unknown properties, so editors
    // flag typos the way loading reports unknown keys.
    auto denies_unknown = [](const codec::dyn::Value& body) {
        const auto* additional = body.get_object()->find("additionalProperties");
        return additional != nullptr && additional->get_bool() == false;
    };
    ZEXPECT(denies_unknown(*doc));
    const auto* defs = doc->get_object()->find("$defs");
    ZASSERT(defs != nullptr);
    for(const auto& [name, body]: *defs->get_object()) {
        ZEXPECT(denies_unknown(body));
    }
}

};  // ZEST_SUITE(Settings)

}  // namespace clice::testing
