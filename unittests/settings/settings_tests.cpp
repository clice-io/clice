module clice;

import :command.database;
import :command.edit;
import :command.table;
import :settings.settings;
import :feature.code_completion;
import :project.build;
import :support.shell;
import :unittests.utils.cdb_helper;
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

/// The edits the rules of `settings` contribute for `path`, the way the build
/// view accumulates them.
void match_rules(const Settings& settings,
                 llvm::StringRef path,
                 std::vector<std::string>& append,
                 std::vector<std::string>& remove) {
    FileTable files;
    CommandTable commands;
    CompilationDatabase cdb{files, commands};
    CanonicalPath root;
    Build build{const_cast<Settings&>(settings), root, cdb, files};
    for(auto& edit: build.edits(CanonicalPath(Spelling::absolute(path))).edits) {
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

ZEST_CASE(CacheDirServesOneProject) {
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
    set_cache_dir_owner(shared, a);
    ZEXPECT(!owned_elsewhere(shared, a));
    ZEXPECT(owned_elsewhere(shared, b));

    // One inside its root belongs to that root, whatever it records, and
    // a subproject nested there finds it taken once the root became its owner.
    tmp.touch("a/.clice/owner", b.str() + "\n");
    auto inner = path::join(a, ".clice");
    ZEXPECT(!owned_elsewhere(inner, a));
    set_cache_dir_owner(inner, a);
    ZEXPECT(owned_elsewhere(inner, CanonicalPath(Spelling::absolute(path::join(a, "sub")))));

    // An owner that no longer exists owns nothing.
    tmp.touch("moved/owner", tmp.path("gone") + "\n");
    auto moved = tmp.path("moved");
    ZEXPECT(!owned_elsewhere(moved, b));
    set_cache_dir_owner(moved, b);
    ZEXPECT(owned_elsewhere(moved, a));

    tmp.touch("b/clice.toml", std::format("[project]\ncache_dir = '{}'\n", shared));
    auto loaded = load_settings(b);
    auto own = path::join(b, ".clice");
    path::canonicalize(own);
    ZEXPECT(std::string(loaded.settings.project.cache_dir) == own);
};

ZEST_CASE(CacheDirFallbackChain) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([project]
cache_dir = "file-cache"
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto identity = [&](llvm::StringRef relative) {
        return CanonicalPath(Spelling::absolute(tmp.path(relative)));
    };
    auto cache_overlay = overlay(R"({ "project": { "cache_dir": "overlay-cache" } })");

    auto merged = load_settings(root, {.overlay = cache_overlay});
    ZEXPECT(std::string_view(merged.settings.project.cache_dir) == identity("overlay-cache").str());

    std::vector<CanonicalPath> taken = {identity("overlay-cache")};
    auto own = load_settings(root, {.overlay = cache_overlay, .taken_cache_dirs = taken});
    ZEXPECT(std::string_view(own.settings.project.cache_dir) == identity("file-cache").str());
    ZEXPECT(own.issues.empty());

    taken.push_back(identity("file-cache"));
    auto fallback = load_settings(root, {.overlay = cache_overlay, .taken_cache_dirs = taken});
    ZEXPECT(std::string_view(fallback.settings.project.cache_dir) == identity(".clice").str());
    ZEXPECT(fallback.settings.project.cache_dir_defaulted.value == true);
    ZEXPECT(fallback.issues.empty());
}

ZEST_CASE(ParsePartialProject) {
    // A partial decode only touches the fields it names; everything else
    // keeps the field-initializer defaults.
    auto result = codec::toml::from_string<ProjectSettings>(R"(cache_dir = "/tmp/test")");
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->cache_dir) == "/tmp/test");
    ZEXPECT(result->enable_indexing.value == true);
    ZEXPECT(result->idle_timeout_ms.value == 3000u);
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
[project]
cache_dir = "/tmp/test"
enable_indexing = false

[[rules]]
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->project.cache_dir) == "/tmp/test");
    ZEXPECT(result->project.enable_indexing.value == false);
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
    ZEXPECT(result->inlay_hints.block_end.value == true);
    ZEXPECT(result->inlay_hints.parameters.value == false);
    ZEXPECT(result->inlay_hints.max_length.types.value == 64u);
    ZEXPECT(result->inlay_hints.max_length.block_end.value == 60u);
    ZEXPECT(result->inlay_hints.designators.value == true);
}

ZEST_CASE(ParseEmptySettings) {
    auto result = codec::toml::from_string<Settings>("");
    ZEXPECT(result);
    ZEXPECT(result->rules.empty());
    ZEXPECT(std::string_view(result->project.cache_dir).empty());
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
    ZEXPECT(std::string_view(result->project.cache_dir).empty());
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
    ZEXPECT(settings.project.enable_indexing.value == true);
    ZEXPECT(settings.project.idle_timeout_ms.value == 3000u);
    ZEXPECT(settings.project.test_hooks.value == false);
    ZEXPECT(settings.project.stateful_worker_count.value == 2u);
    ZEXPECT(settings.project.stateless_worker_count.value >= 2u);
    ZEXPECT(settings.project.min_stateless_worker_count.value == 1u);
    ZEXPECT(settings.project.max_stateless_worker_count.value ==
            default_max_stateless_worker_count());
    ZEXPECT(settings.project.max_stateless_worker_count.value >=
            settings.project.min_stateless_worker_count.value);
    ZEXPECT(settings.tracker.workspace_poll_seconds.value == 30u);
    ZEXPECT(settings.inlay_hints.enabled.value == true);
    ZEXPECT(settings.inlay_hints.parameters.value == true);
    ZEXPECT(settings.inlay_hints.deduced_types.value == true);
    ZEXPECT(settings.inlay_hints.designators.value == true);
    ZEXPECT(settings.inlay_hints.block_end.value == false);
    ZEXPECT(settings.inlay_hints.default_arguments.value == false);
    ZEXPECT(settings.inlay_hints.max_length.types.value == 32u);
    ZEXPECT(settings.code_completion.arguments.value == feature::CompletionArguments::Placeholders);
    ZEXPECT(settings.code_completion.bundle_overloads.value == true);
    ZEXPECT(settings.code_completion.statement_snippets.value == false);
    ZEXPECT(settings.code_completion.dot_to_arrow.value == true);
    ZEXPECT(settings.code_completion.all_scopes.value == true);
    ZEXPECT(settings.code_completion.insert_includes.value == true);
    ZEXPECT(settings.code_completion.insert_imports.value == true);
    ZEXPECT(settings.code_completion.limit.value == 100u);
    ZEXPECT(settings.references.limit.value == 2000u);
}

ZEST_CASE(FinalizeDerivesPaths) {
    Settings settings;
    settings.finalize(CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(!settings.project.cache_dir.empty());
    ZEXPECT(!settings.project.logging_dir.empty());
    ZEXPECT(settings.project.cache_dir_defaulted.value == true);
}

ZEST_CASE(FinalizeEmptyWorkspace) {
    Settings settings;
    settings.finalize(CanonicalPath());
    ZEXPECT(settings.project.cache_dir.empty());
    ZEXPECT(settings.project.logging_dir.empty());
}

ZEST_CASE(FinalizePreservesSet) {
    Settings settings;
    settings.project.cache_dir = "/custom";
    settings.project.enable_indexing = false;
    settings.inlay_hints.parameters = false;
    settings.inlay_hints.block_end = true;
    settings.finalize(CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(std::string_view(settings.project.cache_dir) ==
            CanonicalPath(Spelling::absolute("/custom")).str());
    ZEXPECT(settings.project.cache_dir_defaulted.value == false);
    ZEXPECT(settings.project.enable_indexing.value == false);
    ZEXPECT(settings.inlay_hints.parameters.value == false);
    ZEXPECT(settings.inlay_hints.block_end.value == true);
}

ZEST_CASE(OverlayWithoutFile) {
    TempDir tmp;
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "project": {
            "cache_dir": "/opt/cache",
            "test_hooks": true,
            "enable_indexing": false
        },
        "rules": [
            { "patterns": ["**/*.cpp"], "append": ["-DFOO"] }
        ]
    })")});
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(std::string_view(loaded.settings.project.cache_dir) ==
            CanonicalPath(Spelling("/opt/cache", Spelling::absolute(tmp.root))).str());
    ZEXPECT(loaded.settings.project.enable_indexing.value == false);
    ZEXPECT(loaded.settings.rules.size() == 1u);
    ZEXPECT(loaded.settings.compiled_rules.size() == 1u);
}

ZEST_CASE(LoadMalformedToml) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\nbroken");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.file == path::join(root, "clice.toml"));
    ZEXPECT(loaded.settings.project.enable_indexing.value == true);
}

ZEST_CASE(LegacyProjectKeysIgnored) {
    // Configs written for older clice may still set removed project keys;
    // unknown keys must not fail the parse.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[project]
cache_dir = "/opt/cache"
index_dir = "/opt/index"
worker_memory_limit = 4294967296
)");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!loaded.broken());
    ZEXPECT(std::string_view(loaded.settings.project.cache_dir) ==
            CanonicalPath(Spelling("/opt/cache", Spelling::absolute(tmp.root))).str());
}

ZEST_CASE(MissingFileUsesDefaults) {
    TempDir tmp;
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.file.empty());
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(loaded.settings.project.stateful_worker_count.value == 2u);
}

ZEST_CASE(WorkspaceVarSubst) {
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    Settings settings;
    settings.project.cache_dir = "${workspace}/cache";
    settings.project.logging_dir = "${workspace}/logs";
    settings.rules.push_back(Rule{.compile_commands = {"${workspace}/build"}});
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(std::string_view(settings.project.cache_dir) == at("cache"));
    ZEXPECT(std::string_view(settings.project.logging_dir) == at("logs"));
    ZASSERT(settings.compiled_rules.size() == 1u);
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at("build"));
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
index = false
)");
    ZASSERT(result);
    ZEXPECT(std::string_view(result->default_configuration) == "debug");
    ZASSERT(result->rules.size() == 2u);
    ZEXPECT(std::string_view(result->rules[0].configuration) == "debug");
    ZEXPECT(result->rules[0].compile_commands.size() == 2u);
    ZEXPECT(std::get<std::string>(result->rules[0].default_command) ==
            "clang++ -std=c++20 -Iinclude");
    ZEXPECT(result->rules[0].index);
    ZEXPECT(std::get<std::vector<std::string>>(result->rules[1].default_command).size() == 2u);
    ZEXPECT(!result->rules[1].index);
}

ZEST_CASE(AnchoredRules) {
    /// Relative databases and patterns anchor at the configuration file's
    /// directory, `..` included; `**`-led and absolute patterns match
    /// absolute paths; the top-level databases become the trailing rule.
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
patterns = ["**/*.hxx", "${{workspace}}/gen/**", "../shared/*.cpp", "*"]
append = ["-x", "c++-header"]

[[rules]]
compile_commands = ["build", "{}"]
)",
                          at("elsewhere/compile_commands.json")));

    auto loaded = load_settings(CanonicalPath(Spelling::absolute(root)));
    ZASSERT(!loaded.broken());
    auto& settings = loaded.settings;

    ZASSERT(settings.compiled_rules.size() == 3u);
    ZEXPECT(settings.compiled_rules[0].compile_commands[0].str() == at(".clice/out/debug"));
    ZEXPECT(settings.compiled_rules[0].directory.str() == at(".clice"));
    ZEXPECT(settings.compiled_rules[0].patterns[0].root == identity(".clice/src"));
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
    ZEXPECT(settings.compiled_rules[1].patterns[3].root == identity(".clice"));
    ZEXPECT(settings.compiled_rules[2].patterns.empty());
    ZEXPECT(settings.compiled_rules[2].compile_commands[0].str() == at(".clice/build"));
    ZEXPECT(settings.compiled_rules[2].compile_commands[1].str() ==
            at("elsewhere/compile_commands.json"));

    auto tags = settings.configurations();
    ZASSERT(tags.size() == 1u);
    ZEXPECT(tags[0] == "debug");

    /// A relative pattern sees only files under its anchor; a bare `*`
    /// names the anchor's direct children.
    ZEXPECT(settings.matching_rules(identity(".clice/src/a.cpp"), "debug").size() == 2u);
    ZEXPECT(settings.matching_rules(identity(".clice/src/a.cpp"), "release").size() == 1u);
    ZEXPECT(settings.matching_rules(identity("src/a.cpp"), "debug").size() == 1u);
    ZEXPECT(settings.matching_rules(identity(".clice/a.cpp"), "debug").size() == 2u);
    /// `**` and `${workspace}` patterns match the absolute path; `..`
    /// climbs out of the anchor, `*` stays within one segment.
    auto hxx = settings.matching_rules(identity("other/tree/x.hxx"), "debug");
    ZASSERT(hxx.size() == 2u);
    ZEXPECT(hxx[0]->append.size() == 2u);
    ZEXPECT(settings.matching_rules(identity("gen/x.cpp"), "debug").size() == 2u);
    ZEXPECT(settings.matching_rules(identity("shared/x.cpp"), "debug").size() == 2u);
    ZEXPECT(settings.matching_rules(identity("shared/deep/x.cpp"), "debug").size() == 1u);
}

ZEST_CASE(InitOptionsAnchorAtWorkspace) {
    /// A file under .clice/ anchors its own paths there; values overlaid
    /// through initializationOptions anchor at the workspace root, whatever
    /// file was loaded before them.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    std::string root = tmp.root;
    path::canonicalize(root);
    tmp.touch(".clice/config.toml", R"(
[[rules]]
patterns = ["../src/**"]
append = ["-DFROM_FILE"]

[[rules]]
compile_commands = ["../build"]
)");

    auto from_file = load_settings(CanonicalPath(Spelling::absolute(root))).settings;
    ZASSERT(from_file.compiled_rules.size() == 2u);
    ZEXPECT(from_file.compiled_rules[0].patterns[0].root ==
            CanonicalPath(Spelling::absolute(at("src"))));
    ZEXPECT(from_file.compiled_rules[0].directory.str() == at(".clice"));
    ZEXPECT(from_file.compiled_rules[1].compile_commands[0].str() == at(".clice/../build"));

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
    ZEXPECT(settings.compiled_rules[0].directory.str() == root);
    ZEXPECT(settings.compiled_rules[1].compile_commands[0].str() == at("out"));
}

ZEST_CASE(RelativeProjectDirsAnchor) {
    /// A relative cache or log directory anchors where every other relative
    /// path does: at its configuration file, or at the workspace root when
    /// it came through initializationOptions.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    std::string root = tmp.root;
    path::canonicalize(root);
    tmp.touch(".clice/config.toml", "[project]\ncache_dir = \"cache\"\n");

    auto loaded = load_settings(CanonicalPath(Spelling::absolute(root)));
    ZASSERT(!loaded.broken());
    ZEXPECT(std::string_view(loaded.settings.project.cache_dir) == at(".clice/cache"));
    ZEXPECT(std::string_view(loaded.settings.project.logging_dir) == at(".clice/cache/logs"));

    Settings settings;
    settings.project.logging_dir = "logs";
    settings.finalize(CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(std::string_view(settings.project.logging_dir) == at("logs"));
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
                      {.overlay = overlay(R"({ "project": { "idle_timeout_ms": 42 } })")});
    ZEXPECT(loaded.issues.empty());
    ZEXPECT(loaded.settings.project.idle_timeout_ms.value == 42u);
    // Unset fields still receive defaults.
    ZEXPECT(loaded.settings.project.enable_indexing.value == true);
    ZEXPECT(loaded.settings.project.stateful_worker_count.value == 2u);
}

ZEST_CASE(DefaultWorkspaceCache) {
    Settings settings;
    CanonicalPath root(Spelling::absolute("/ws/root"));
    settings.finalize(root);

    ZEXPECT(std::string_view(settings.project.cache_dir) == root.str() + "/.clice");
    ZEXPECT(std::string_view(settings.project.logging_dir) == root.str() + "/.clice/logs");
}

ZEST_CASE(WorkspaceSubstEmpty) {
    // Empty project_root must not rewrite "${workspace}" into "" and produce
    // bogus paths like "/cache": the directory stays relative, and a project
    // without a folder has nothing to anchor it at, so it is dropped.
    Settings settings;
    settings.project.cache_dir = "${workspace}/cache";
    settings.finalize(CanonicalPath());
    ZEXPECT(settings.project.cache_dir.empty());
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
    settings.project.cache_dir = "~/clice-cache";
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    std::string home;
    ZASSERT(!sys::home_dir(home));
    ZEXPECT(std::string_view(settings.project.cache_dir) ==
            CanonicalPath(Spelling("clice-cache", Spelling::absolute(home))).str());
}

ZEST_CASE(WorkspaceSubstRepeated) {
    // Multiple ${workspace} occurrences in one string all get substituted.
    Settings settings;
    settings.project.cache_dir = "${workspace}/a/${workspace}/b";
    CanonicalPath root(Spelling::absolute("/root"));
    settings.finalize(root);
    ZEXPECT(std::string_view(settings.project.cache_dir) ==
            CanonicalPath(Spelling::absolute(root.str() + "/a/" + root.str() + "/b")).str());
}

ZEST_CASE(CompileCommandsList) {
    // Every top-level database path substitutes ${workspace} and anchors
    // at the configuration directory; absolute ones pass through, and an
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

ZEST_CASE(TomlErrorLocated) {
    // A syntax error sits where the parser stopped: the line break that
    // ends the table header before its `]`.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
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
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
}

ZEST_CASE(TypeIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\ntest_hooks = \"yes\"\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(loaded.issues[0].message.find("test_hooks") != std::string::npos);
}

ZEST_CASE(RemovedKeyIssueWarns) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nworker_memory_limit = 4294967296\n");
    auto loaded = load_settings(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Warning);
    ZEXPECT(loaded.issues[0].message.find("worker_memory_limit") != std::string::npos);
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
    tmp.touch("clice.toml", R"([project]
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
    ZEXPECT(loaded.settings.project.readonly.value == ReadonlyMode::Off);
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

ZEST_CASE(ZeroWorkerCountRejected) {
    // An explicit 0 is not a runnable worker configuration: finalize
    // reports it and, meanwhile, falls back to the born-valid default
    // instead of starting a pool with no workers.
    Settings settings;
    settings.project.stateful_worker_count = 0;
    settings.project.stateless_worker_count = 0;
    settings.project.min_stateless_worker_count = 0;
    auto problems = settings.finalize(CanonicalPath());
    ZASSERT(problems.size() == 3u);
    ZEXPECT(problems[0].to_string() ==
            "a worker count must be at least 1 at project.stateful_worker_count");
    ZEXPECT(problems[1].to_string() ==
            "a worker count must be at least 1 at project.stateless_worker_count");
    ZEXPECT(problems[2].to_string() ==
            "a worker count must be at least 1 at project.min_stateless_worker_count");
    ZEXPECT(settings.project.stateful_worker_count.value == 2u);
    ZEXPECT(settings.project.stateless_worker_count.value >= 2u);
    ZEXPECT(settings.project.min_stateless_worker_count.value == 1u);
}

ZEST_CASE(ZeroWorkerCountLocated) {
    TempDir tmp;
    tmp.touch("clice.toml", R"([project]
stateful_worker_count = 0
)");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file == path::join(root, "clice.toml"));
    ZEXPECT(issue.message.find("stateful_worker_count") != std::string::npos);
    ZEXPECT(issue.line == 2u);
    ZEXPECT(issue.column == 25u);
    ZEXPECT(loaded.settings.project.stateful_worker_count.value == 2u);
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
        load_settings(root, {.overlay = overlay(R"({ "project": { "test_hooks": null } })")});
    ZEXPECT(flat.broken());
    auto nested =
        load_settings(root, {.overlay = overlay(R"({ "inlay_hints": { "block_end": null } })")});
    ZEXPECT(nested.broken());
}

ZEST_CASE(OverlayUnknownKeyWarns) {
    TempDir tmp;
    auto loaded = load_settings(
        CanonicalPath(Spelling::absolute(tmp.root)),
        {.overlay = overlay(R"({ "project": { "idel_timeout_ms": 5, "idle_timeout_ms": 42 } })")});
    ZEXPECT(!loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Warning);
    ZEXPECT(issue.file.empty());
    ZEXPECT(issue.message.starts_with("initializationOptions: "));
    ZEXPECT(issue.message.find("idel_timeout_ms") != std::string::npos);
    ZEXPECT(loaded.settings.project.idle_timeout_ms.value == 42u);
}

ZEST_CASE(BadOverlayAppliesNothing) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nidle_timeout_ms = 16\n");
    auto loaded =
        load_settings(CanonicalPath(Spelling::absolute(tmp.root)), {.overlay = overlay(R"({
        "rules": [{ "patterns": ["**/*.cpp"], "append": ["-DOVERLAY"] }],
        "project": { "idle_timeout_ms": 99, "enable_indexing": "yes" }
    })")});
    ZEXPECT(loaded.broken());
    ZASSERT(loaded.issues.size() == 1u);
    auto& issue = loaded.issues[0];
    ZEXPECT(issue.severity == SettingsIssue::Severity::Error);
    ZEXPECT(issue.file.empty());
    ZEXPECT(issue.message.starts_with("initializationOptions: "));
    ZEXPECT(issue.message.find("project.enable_indexing") != std::string::npos);
    ZEXPECT(loaded.settings.rules.empty());
    ZEXPECT(loaded.settings.project.idle_timeout_ms.value == 16u);
    ZEXPECT(loaded.settings.project.enable_indexing.value == true);
}

ZEST_CASE(MalformedTomlNoFallback) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ninvalid");
    tmp.touch(".clice/config.toml", "[project]\nenable_indexing = false\n");
    CanonicalPath root(Spelling::absolute(tmp.root));
    auto loaded = load_settings(root);
    ZEXPECT(loaded.broken());
    ZEXPECT(loaded.file == path::join(root, "clice.toml"));
    ZASSERT(loaded.issues.size() == 1u);
    ZEXPECT(loaded.issues[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(loaded.issues[0].file == path::join(root, "clice.toml"));
    ZEXPECT(loaded.settings.project.enable_indexing.value == true);
    ZEXPECT(loaded.settings.project.stateful_worker_count.value == 2u);
}

ZEST_CASE(RuleOrderLaterRemoveWins) {
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
    Build build{settings, project_root, cdb, files};
    auto edits = build.edits(CanonicalPath(Spelling::absolute("/src/a.cpp")));
    ZASSERT(edits.edits.size() == 2u);
    ZEXPECT(edits.edits[1].kind == CommandEdit::Kind::Remove);
    ZEXPECT(print_argv(render_entry(cdb, "/src/a.cpp", edits.options())) ==
            "clang++ -D BAR " + CanonicalPath(Spelling::absolute("/src/a.cpp")).str());
}

ZEST_CASE(RuleOrderLaterAppendWins) {
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

ZEST_CASE(InitOptionsOverlayPreservesToml) {
    // Mirror the master_server flow: load the project settings from clice.toml first,
    // then overlay initializationOptions JSON. Fields absent in the JSON must
    // keep their clice.toml values; fields present in the JSON override.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[project]
cache_dir = "/from/toml"
test_hooks = true
idle_timeout_ms = 16

[[rules]]
patterns = ["**/*.cpp"]
append = ["-DFROM_TOML"]
)");

    CanonicalPath root(Spelling::absolute(tmp.root));
    auto from_file = load_settings(root).settings;
    auto from_toml = CanonicalPath(Spelling("/from/toml", Spelling::absolute(tmp.root))).str();
    ZEXPECT(std::string_view(from_file.project.cache_dir) == from_toml);
    ZEXPECT(from_file.project.test_hooks.value == true);
    ZEXPECT(from_file.project.idle_timeout_ms.value == 16u);
    ZEXPECT(from_file.compiled_rules.size() == 1u);

    // Overlay only `idle_timeout_ms` via JSON.
    auto overlaid =
        load_settings(root, {.overlay = overlay(R"({ "project": { "idle_timeout_ms": 99 } })")});
    ZEXPECT(overlaid.issues.empty());
    auto& settings = overlaid.settings;

    // Overridden field.
    ZEXPECT(settings.project.idle_timeout_ms.value == 99u);
    // Untouched fields stay at TOML values.
    ZEXPECT(std::string_view(settings.project.cache_dir) == from_toml);
    ZEXPECT(settings.project.test_hooks.value == true);
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
    ZEXPECT(settings.code_completion.bundle_overloads.value == false);
    // From the JSON overlay, including a nested field both layers set —
    // the later source wins.
    ZEXPECT(settings.inlay_hints.block_end.value == false);
    ZEXPECT(settings.inlay_hints.parameters.value == false);
    ZEXPECT(settings.code_completion.limit.value == 5u);
    // Named by nobody: field-initializer defaults.
    ZEXPECT(settings.inlay_hints.deduced_types.value == true);
    ZEXPECT(settings.code_completion.arguments.value == feature::CompletionArguments::Placeholders);
}

ZEST_CASE(InitOptionsOverlayRulesReplace) {
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

    // Machine-derived worker counts describe their derivation but carry no
    // default value anywhere — neither in their own schema nor inside a
    // section's whole-object default — so the schema stays byte-identical
    // across hosts.
    for(auto field: {"stateless_worker_count", "max_stateless_worker_count"}) {
        const auto* worker = find_property(*doc, field);
        ZASSERT(worker != nullptr);
        const auto* object = worker->get_object();
        ZASSERT(object != nullptr);
        ZEXPECT(object->find("description") != nullptr);
        ZEXPECT(object->find("default") == nullptr);
        ZEXPECT(!default_mentions(*doc, field, false));
    }
    // Control: a stable field does appear under the section default.
    ZEXPECT(default_mentions(*doc, "idle_timeout_ms", false));

    // A stable field initializer survives as the schema default, and the
    // unsigned field type keeps negative delays out of the schema.
    const auto* idle = find_property(*doc, "idle_timeout_ms");
    ZASSERT(idle != nullptr);
    ZEXPECT(idle->get_object()->find("default") != nullptr);
    const auto* idle_minimum = idle->get_object()->find("minimum");
    ZASSERT(idle_minimum != nullptr);
    ZEXPECT(idle_minimum->get_uint().value_or(1) == 0u);

    // skip = true fields stay out of the schema entirely.
    ZEXPECT(find_property(*doc, "compiled_rules") == nullptr);

    // The fields finalize() rejects `0` for carry the matching lower bound.
    for(auto field:
        {"stateful_worker_count", "stateless_worker_count", "min_stateless_worker_count"}) {
        const auto* property = find_property(*doc, field);
        ZASSERT(property != nullptr);
        const auto* minimum = property->get_object()->find("minimum");
        ZASSERT(minimum != nullptr);
        ZEXPECT(minimum->get_uint().value_or(0) == 1u);
    }

    // Enum fields name their accepted values, so editors flag a typo that
    // the lenient decode would silently turn into the default.
    const auto* readonly = find_property(*doc, "readonly");
    ZASSERT(readonly != nullptr);
    const auto* modes = readonly->get_object()->find("enum");
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
