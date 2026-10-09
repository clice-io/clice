module;

#include "modules/prelude.h"

module clice;

import :config.config;
import :project.build;
import :tests.unit.test.cdb_helper;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.path;

namespace clice::testing {

/// The schema object of a property named `name`, found anywhere in the
/// document's nested `properties` maps.
const static kota::codec::dyn::Value* find_property(const kota::codec::dyn::Value& value,
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
static bool default_mentions(const kota::codec::dyn::Value& value,
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

/// The edits the rules of `config` contribute for `path`, the way the build
/// view accumulates them.
void match_rules(const Config& config,
                 llvm::StringRef path,
                 std::vector<std::string>& append,
                 std::vector<std::string>& remove) {
    FileTable files;
    CompilationDatabase cdb{files};
    Build build{const_cast<Config&>(config), cdb, files};
    for(auto& edit: build.edits(CanonicalPath(Spelling::absolute(path))).edits) {
        auto& out = edit.kind == CommandEdit::Kind::Remove ? remove : append;
        out.insert(out.end(), edit.flags.begin(), edit.flags.end());
    }
}

ZEST_SUITE(Config) {

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
    claim_cache_dir(shared, a);
    ZEXPECT(!owned_elsewhere(shared, a));
    ZEXPECT(owned_elsewhere(shared, b));

    // One inside its root belongs to that root, whatever it records, and
    // a subproject nested there finds it taken once the root claimed it.
    tmp.touch("a/.clice/owner", b.str() + "\n");
    auto inner = path::join(a, ".clice");
    ZEXPECT(!owned_elsewhere(inner, a));
    claim_cache_dir(inner, a);
    ZEXPECT(owned_elsewhere(inner, CanonicalPath(Spelling::absolute(path::join(a, "sub")))));

    // An owner that no longer exists claims nothing.
    tmp.touch("moved/owner", tmp.path("gone") + "\n");
    auto moved = tmp.path("moved");
    ZEXPECT(!owned_elsewhere(moved, b));
    claim_cache_dir(moved, b);
    ZEXPECT(owned_elsewhere(moved, a));

    tmp.touch("b/clice.toml", std::format("[project]\ncache_dir = '{}'\n", shared));
    auto config = Config::load_from_workspace(b);
    auto own = path::join(b, ".clice");
    path::canonicalize(own);
    ZEXPECT(std::string(config.project.cache_dir) == own);
};

ZEST_CASE(ParsePartialProject) {
    // A partial decode only touches the fields it names; everything else
    // keeps the field-initializer defaults.
    auto result = kota::codec::toml::from_string<ProjectConfig>(R"(cache_dir = "/tmp/test")");
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->cache_dir) == "/tmp/test");
    ZEXPECT(result->enable_indexing.value == true);
    ZEXPECT(result->idle_timeout_ms.value == 3000u);
}

ZEST_CASE(ParseConfigRule) {
    auto result = kota::codec::toml::from_string<ConfigRule>(R"(
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    ZEXPECT(result);
    ZEXPECT(result->patterns.size() == 1u);
    ZEXPECT(result->patterns[0] == "**/*.cpp");
    ZEXPECT(result->append[0] == "-std=c++20");
    ZEXPECT(result->remove.empty());
}

ZEST_CASE(ParseFullConfig) {
    auto result = kota::codec::toml::from_string<Config>(R"(
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
    auto result = kota::codec::toml::from_string<Config>(R"(
[inlay_hints]
block_end = true
parameters = false
type_name_limit = 64
)");
    ZEXPECT(result);
    ZEXPECT(result->inlay_hints.block_end.value == true);
    ZEXPECT(result->inlay_hints.parameters.value == false);
    ZEXPECT(result->inlay_hints.type_name_limit.value == 64u);
    ZEXPECT(result->inlay_hints.designators.value == true);
}

ZEST_CASE(ParseEmptyConfig) {
    auto result = kota::codec::toml::from_string<Config>("");
    ZEXPECT(result);
    ZEXPECT(result->rules.empty());
    ZEXPECT(std::string_view(result->project.cache_dir).empty());
}

ZEST_CASE(ParseOnlyRules) {
    auto result = kota::codec::toml::from_string<Config>(R"(
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
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-std=c++20"},
        .remove = {"-std=c++17"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.cpp", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-std=c++20");
    ZEXPECT(remove.size() == 1u);
    ZEXPECT(remove[0] == "-std=c++17");
}

ZEST_CASE(MatchRulesNoMatch) {
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.h", append, remove);
    ZEXPECT(append.empty());
    ZEXPECT(remove.empty());
}

ZEST_CASE(MatchRulesMultiple) {
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DCPP"},
    });
    config.rules.push_back(ConfigRule{
        .patterns = {"**/test_*.cpp"},
        .append = {"-DTEST"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(config, "/src/test_foo.cpp", append, remove);
    ZEXPECT(append.size() == 2u);
    ZEXPECT(append[0] == "-DCPP");
    ZEXPECT(append[1] == "-DTEST");
}

ZEST_CASE(BornValidDefaults) {
    // A default-constructed Config is fully valid without any init step;
    // the option defaults come from the field initializers alone.
    Config config;
    ZEXPECT(config.project.enable_indexing.value == true);
    ZEXPECT(config.project.idle_timeout_ms.value == 3000u);
    ZEXPECT(config.project.test_hooks.value == false);
    ZEXPECT(config.project.stateful_worker_count.value == 2u);
    ZEXPECT(config.project.stateless_worker_count.value >= 2u);
    ZEXPECT(config.project.min_stateless_worker_count.value == 1u);
    ZEXPECT(config.project.max_stateless_worker_count.value ==
            default_max_stateless_worker_count());
    ZEXPECT(config.project.max_stateless_worker_count.value >=
            config.project.min_stateless_worker_count.value);
    ZEXPECT(config.tracker.workspace_poll_seconds.value == 30u);
    ZEXPECT(config.inlay_hints.enabled.value == true);
    ZEXPECT(config.inlay_hints.parameters.value == true);
    ZEXPECT(config.inlay_hints.deduced_types.value == true);
    ZEXPECT(config.inlay_hints.designators.value == true);
    ZEXPECT(config.inlay_hints.block_end.value == false);
    ZEXPECT(config.inlay_hints.default_arguments.value == false);
    ZEXPECT(config.inlay_hints.type_name_limit.value == 32u);
    ZEXPECT(config.code_completion.enable_keyword_snippet.value == false);
    ZEXPECT(config.code_completion.enable_function_arguments_snippet.value == false);
    ZEXPECT(config.code_completion.enable_template_arguments_snippet.value == false);
    ZEXPECT(config.code_completion.insert_paren_in_function_call.value == false);
    ZEXPECT(config.code_completion.bundle_overloads.value == true);
    ZEXPECT(config.code_completion.limit.value == 0u);
}

ZEST_CASE(FinalizeDerivesPaths) {
    Config config;
    config.finalize(CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(!config.project.cache_dir.empty());
    ZEXPECT(!config.project.logging_dir.empty());
    ZEXPECT(config.project.cache_dir_defaulted.value == true);
}

ZEST_CASE(FinalizeEmptyWorkspace) {
    Config config;
    config.finalize(CanonicalPath());
    ZEXPECT(config.project.cache_dir.empty());
    ZEXPECT(config.project.logging_dir.empty());
}

ZEST_CASE(FinalizePreservesSet) {
    Config config;
    config.project.cache_dir = "/custom";
    config.project.enable_indexing = false;
    config.inlay_hints.parameters = false;
    config.inlay_hints.block_end = true;
    config.finalize(CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(std::string_view(config.project.cache_dir) ==
            CanonicalPath(Spelling::absolute("/custom")).str());
    ZEXPECT(config.project.cache_dir_defaulted.value == false);
    ZEXPECT(config.project.enable_indexing.value == false);
    ZEXPECT(config.inlay_hints.parameters.value == false);
    ZEXPECT(config.inlay_hints.block_end.value == true);
}

ZEST_CASE(LoadFromJson) {
    auto result = Config::load_from_json(R"({
        "project": {
            "cache_dir": "/opt/cache",
            "test_hooks": true,
            "enable_indexing": false
        },
        "rules": [
            { "patterns": ["**/*.cpp"], "append": ["-DFOO"] }
        ]
    })",
                                         CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->project.cache_dir) ==
            CanonicalPath(Spelling::absolute("/opt/cache")).str());
    ZEXPECT(result->project.enable_indexing.value == false);
    ZEXPECT(result->rules.size() == 1u);
    ZEXPECT(result->compiled_rules.size() == 1u);
}

ZEST_CASE(LoadFromJsonInvalid) {
    auto result =
        Config::load_from_json("{not valid json", CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(!result.has_value());
}

ZEST_CASE(LoadMalformedToml) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\nbroken");
    auto result = Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!result.has_value());
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
    auto result = Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(result);
    ZEXPECT(std::string_view(result->project.cache_dir) ==
            CanonicalPath(Spelling("/opt/cache", Spelling::absolute(tmp.root))).str());
}

ZEST_CASE(LoadMissingFile) {
    auto result =
        Config::load("/nonexistent/clice.toml", CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(!result.has_value());
}

ZEST_CASE(WorkspaceVarSubst) {
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
    };
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.project.logging_dir = "${workspace}/logs";
    config.rules.push_back(ConfigRule{.compile_commands = {"${workspace}/build"}});
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(std::string_view(config.project.cache_dir) == at("cache"));
    ZEXPECT(std::string_view(config.project.logging_dir) == at("logs"));
    ZASSERT(config.compiled_rules.size() == 1u);
    ZEXPECT(config.compiled_rules[0].compile_commands[0].str() == at("build"));
}

ZEST_CASE(ParseRuleSources) {
    auto result = kota::codec::toml::from_string<Config>(R"(
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
    std::string root = tmp.root.str().str();
    path::canonicalize(root);
    tmp.touch("sub/clice.toml",
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

    auto loaded = Config::load(tmp.path("sub/clice.toml"), CanonicalPath(Spelling::absolute(root)));
    ZASSERT(loaded);
    auto& config = *loaded;

    ZASSERT(config.compiled_rules.size() == 3u);
    ZEXPECT(config.compiled_rules[0].compile_commands[0].str() == at("sub/out/debug"));
    ZEXPECT(config.compiled_rules[0].directory.str() == at("sub"));
    ZEXPECT(config.compiled_rules[0].patterns[0].root == identity("sub/src"));
    ZEXPECT(config.compiled_rules[0].declares_sources());
    /// The string spelling is tokenized, then `${workspace}` substituted
    /// per argument.
    ZASSERT(config.compiled_rules[0].default_command.size() == 3u);
    ZEXPECT(config.compiled_rules[0].default_command[2] == "-I" + at("include"));
    ZEXPECT(!config.compiled_rules[1].declares_sources());
    ZASSERT(config.compiled_rules[1].patterns.size() == 4u);
    ZEXPECT(config.compiled_rules[1].patterns[0].root == CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(config.compiled_rules[1].patterns[1].root == identity("gen"));
    ZEXPECT(config.compiled_rules[1].patterns[2].root == identity("shared"));
    ZEXPECT(config.compiled_rules[1].patterns[3].root == identity("sub"));
    ZEXPECT(config.compiled_rules[2].patterns.empty());
    ZEXPECT(config.compiled_rules[2].compile_commands[0].str() == at("sub/build"));
    ZEXPECT(config.compiled_rules[2].compile_commands[1].str() ==
            at("elsewhere/compile_commands.json"));

    auto tags = config.configurations();
    ZASSERT(tags.size() == 1u);
    ZEXPECT(tags[0] == "debug");

    /// A relative pattern sees only files under its anchor; a bare `*`
    /// names the anchor's direct children.
    ZEXPECT(config.matching_rules(identity("sub/src/a.cpp"), "debug").size() == 2u);
    ZEXPECT(config.matching_rules(identity("sub/src/a.cpp"), "release").size() == 1u);
    ZEXPECT(config.matching_rules(identity("src/a.cpp"), "debug").size() == 1u);
    ZEXPECT(config.matching_rules(identity("sub/a.cpp"), "debug").size() == 2u);
    /// `**` and `${workspace}` patterns match the absolute path; `..`
    /// climbs out of the anchor, `*` stays within one segment.
    auto hxx = config.matching_rules(identity("other/tree/x.hxx"), "debug");
    ZASSERT(hxx.size() == 2u);
    ZEXPECT(hxx[0]->append.size() == 2u);
    ZEXPECT(config.matching_rules(identity("gen/x.cpp"), "debug").size() == 2u);
    ZEXPECT(config.matching_rules(identity("shared/x.cpp"), "debug").size() == 2u);
    ZEXPECT(config.matching_rules(identity("shared/deep/x.cpp"), "debug").size() == 1u);
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
    std::string root = tmp.root.str().str();
    path::canonicalize(root);
    tmp.touch(".clice/config.toml", R"(
[[rules]]
patterns = ["../src/**"]
append = ["-DFROM_FILE"]

[[rules]]
compile_commands = ["../build"]
)");

    auto from_file = Config::load_from_workspace(CanonicalPath(Spelling::absolute(root)));
    ZASSERT(from_file.compiled_rules.size() == 2u);
    ZEXPECT(from_file.compiled_rules[0].patterns[0].root ==
            CanonicalPath(Spelling::absolute(at("src"))));
    ZEXPECT(from_file.compiled_rules[0].directory.str() == at(".clice"));
    ZEXPECT(from_file.compiled_rules[1].compile_commands[0].str() == at(".clice/../build"));

    auto config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(root)),
                                              nullptr,
                                              nullptr,
                                              /*finalized=*/false);
    auto ov = kota::codec::json::from_string(
        R"({ "rules": [{ "patterns": ["src/**"], "compile_commands": ["cmake"] }, { "compile_commands": ["out"] }] })",
        config);
    ZASSERT(ov);
    config.finalize(CanonicalPath(Spelling::absolute(root)));

    ZASSERT(config.compiled_rules.size() == 2u);
    ZEXPECT(config.compiled_rules[0].patterns[0].root ==
            CanonicalPath(Spelling::absolute(at("src"))));
    ZEXPECT(config.compiled_rules[0].compile_commands[0].str() == at("cmake"));
    ZEXPECT(config.compiled_rules[0].directory.str() == root);
    ZEXPECT(config.compiled_rules[1].compile_commands[0].str() == at("out"));
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
    std::string root = tmp.root.str().str();
    path::canonicalize(root);
    tmp.touch("sub/clice.toml", "[project]\ncache_dir = \"cache\"\n");

    auto loaded = Config::load(tmp.path("sub/clice.toml"), CanonicalPath(Spelling::absolute(root)));
    ZASSERT(loaded);
    ZEXPECT(std::string_view(loaded->project.cache_dir) == at("sub/cache"));
    ZEXPECT(std::string_view(loaded->project.logging_dir) == at("sub/cache/logs"));

    Config config;
    config.project.logging_dir = "logs";
    config.finalize(CanonicalPath(Spelling::absolute(root)));
    ZEXPECT(std::string_view(config.project.logging_dir) == at("logs"));
}

ZEST_CASE(SourcesOffByDefault) {
    Config config;
    config.rules.push_back(ConfigRule{.patterns = {"**/*"}, .append = {"-DX"}});
    config.finalize(CanonicalPath(Spelling::absolute("/ws")));
    ZEXPECT(!config.compiled_rules[0].declares_sources());
    ZEXPECT(config.configurations().empty());
}

ZEST_CASE(InvalidGlobPattern) {
    Config config;
    // All-invalid patterns: the rule matches nothing (its flags never
    // apply), but the database it declares still loads.
    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}"},
        .compile_commands = {"/elsewhere/compile_commands.json"},
        .append = {"-DSHOULD_NOT_APPEAR"},
    });
    // Mixed valid/invalid: only the invalid pattern is skipped; rule remains.
    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}", "**/*.cpp"},
        .append = {"-DCPP"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));
    ZASSERT(config.compiled_rules.size() == 2u);
    ZEXPECT(config.compiled_rules[0].unmatchable);
    ZEXPECT(config.compiled_rules[0].declares_sources());
    ZEXPECT(!config.compiled_rules[1].unmatchable);

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.cpp", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-DCPP");
}

ZEST_CASE(ConfigPriorityJson) {
    // initializationOptions-sourced config should override an on-disk default.
    auto from_json = Config::load_from_json(R"({ "project": { "idle_timeout_ms": 42 } })",
                                            CanonicalPath(Spelling::absolute("/workspace")));
    ZEXPECT(from_json);
    ZEXPECT(from_json->project.idle_timeout_ms.value == 42u);
    // Unset fields still receive defaults.
    ZEXPECT(from_json->project.enable_indexing.value == true);
    ZEXPECT(from_json->project.stateful_worker_count.value == 2u);
}

ZEST_CASE(DefaultWorkspaceCache) {
    Config config;
    CanonicalPath root(Spelling::absolute("/ws/root"));
    config.finalize(root);

    ZEXPECT(std::string_view(config.project.cache_dir) == root.str() + "/.clice");
    ZEXPECT(std::string_view(config.project.logging_dir) == root.str() + "/.clice/logs");
}

ZEST_CASE(WorkspaceSubstEmpty) {
    // Empty workspace_root must not rewrite "${workspace}" into "" and produce
    // bogus paths like "/cache": the directory stays relative, and a project
    // without a folder has nothing to anchor it at, so it is dropped.
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.finalize(CanonicalPath());
    ZEXPECT(config.project.cache_dir.empty());
}

ZEST_CASE(RootlessRulesIgnored) {
    // A server without folders has nothing to anchor a rule's paths at:
    // the rule is dropped instead of matching against the process's cwd.
    Config config;
    config.rules.push_back(ConfigRule{.patterns = {"src/**"}, .append = {"-DX"}});
    config.finalize(CanonicalPath());
    ZEXPECT(config.compiled_rules.empty());
}

ZEST_CASE(HomeExpanded) {
    // A leading `~` is the home directory, not a directory named `~`.
    TempDir tmp;
    Config config;
    config.project.cache_dir = "~/clice-cache";
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    llvm::SmallString<256> home;
    ZASSERT(llvm::sys::path::home_directory(home));
    ZEXPECT(std::string_view(config.project.cache_dir) ==
            CanonicalPath(Spelling("clice-cache", Spelling::absolute(home))).str());
}

ZEST_CASE(WorkspaceSubstRepeated) {
    // Multiple ${workspace} occurrences in one string all get substituted.
    Config config;
    config.project.cache_dir = "${workspace}/a/${workspace}/b";
    CanonicalPath root(Spelling::absolute("/root"));
    config.finalize(root);
    ZEXPECT(std::string_view(config.project.cache_dir) ==
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
    Config config;
    config.rules.push_back(ConfigRule{
        .compile_commands = {
                             "${workspace}/build", at("abs/path/compile_commands.json"),
                             "out", "build.json",
                             }
    });
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZASSERT(config.compiled_rules.size() == 1u);
    auto& databases = config.compiled_rules[0].compile_commands;
    ZASSERT(databases.size() == 4u);
    ZEXPECT(databases[0].str() == at("build"));
    ZEXPECT(databases[1].str() == at("abs/path/compile_commands.json"));
    ZEXPECT(databases[2].str() == at("out"));
    ZEXPECT(databases[3].str() == at("build.json/compile_commands.json"));
}

ZEST_CASE(TomlErrorLocated) {
    // Malformed TOML (bad table header, missing close-bracket) must return nullopt.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
    auto result = Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(!result.has_value());
}

// FIXME: assert ConfigIssue::line/column once kotatsu's TOML decoder exposes
// error locations (feature 60661c1 on the unmerged kotatsu branch); the
// plumbing here already forwards rich_error.location when present.
ZEST_CASE(SyntaxIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
    std::vector<ConfigIssue> issues;
    auto result =
        Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)), &issues);
    ZEXPECT(!result.has_value());
    ZASSERT(issues.size() == 1u);
    ZEXPECT(issues[0].severity == ConfigIssue::Severity::Error);
}

ZEST_CASE(TypeIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\ntest_hooks = \"yes\"\n");
    std::vector<ConfigIssue> issues;
    auto result =
        Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)), &issues);
    ZEXPECT(!result.has_value());
    ZASSERT(issues.size() == 1u);
    ZEXPECT(issues[0].severity == ConfigIssue::Severity::Error);
    ZEXPECT(issues[0].message.find("test_hooks") != std::string::npos);
}

ZEST_CASE(RemovedKeyIssueWarns) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nworker_memory_limit = 4294967296\n");
    std::vector<ConfigIssue> issues;
    auto result =
        Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)), &issues);
    ZEXPECT(result);
    ZASSERT(issues.size() == 1u);
    ZEXPECT(issues[0].severity == ConfigIssue::Severity::Warning);
    ZEXPECT(issues[0].message.find("worker_memory_limit") != std::string::npos);
}

ZEST_CASE(UnknownFeatureKeyWarns) {
    // Feature options structs double as config sections; a typo inside one
    // must still surface as a Warning, not vanish.
    TempDir tmp;
    tmp.touch("clice.toml", "[inlay_hints]\nblokc_end = true\n");
    std::vector<ConfigIssue> issues;
    auto result =
        Config::load(tmp.path("clice.toml"), CanonicalPath(Spelling::absolute(tmp.root)), &issues);
    ZEXPECT(result);
    ZASSERT(issues.size() == 1u);
    ZEXPECT(issues[0].severity == ConfigIssue::Severity::Warning);
    ZEXPECT(issues[0].message.find("blokc_end") != std::string::npos);
}

ZEST_CASE(ZeroWorkerCountRejected) {
    // An explicit 0 is not a runnable worker configuration; finalize
    // validates it back to the born-valid default instead of starting a
    // pool with no workers.
    Config config;
    config.project.stateful_worker_count = 0;
    config.project.stateless_worker_count = 0;
    config.project.min_stateless_worker_count = 0;
    config.finalize(CanonicalPath());
    ZEXPECT(config.project.stateful_worker_count.value == 2u);
    ZEXPECT(config.project.stateless_worker_count.value >= 2u);
    ZEXPECT(config.project.min_stateless_worker_count.value == 1u);
}

ZEST_CASE(NullOptionRejected) {
    // `defaulted` means "may be absent", never "may be null": an explicit
    // JSON null on an option is a type error and fails the whole decode,
    // both flat and inside a feature section.
    auto flat = Config::load_from_json(R"({ "project": { "test_hooks": null } })",
                                       CanonicalPath(Spelling::absolute("/ws")));
    ZEXPECT(!flat.has_value());
    auto nested = Config::load_from_json(R"({ "inlay_hints": { "block_end": null } })",
                                         CanonicalPath(Spelling::absolute("/ws")));
    ZEXPECT(!nested.has_value());
}

ZEST_CASE(WorkspaceMalformedFallback) {
    // load_from_workspace must fall back to defaults when clice.toml is malformed,
    // not propagate the failure.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ninvalid");
    auto config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.root)));
    // Defaults still applied.
    ZEXPECT(config.project.stateful_worker_count.value == 2u);
    ZEXPECT(config.project.enable_indexing.value == true);
}

ZEST_CASE(RuleOrderLaterRemoveWins) {
    // Later rule's `remove` must cancel an earlier rule's matching `append`.
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO", "-DBAR"},
    });
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .remove = {"-DFOO"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));

    // The edits stay in rule order; the remove takes effect against the
    // earlier append when the command is built, and against the base.
    FileTable files;
    CompilationDatabase cdb{files};
    cdb.add_command("/src", "/src/a.cpp", std::string_view("clang++ -DFOO a.cpp"));
    Build build{config, cdb, files};
    auto edits = build.edits(CanonicalPath(Spelling::absolute("/src/a.cpp")));
    ZASSERT(edits.edits.size() == 2u);
    ZEXPECT(edits.edits[1].kind == CommandEdit::Kind::Remove);
    ZEXPECT(print_argv(render_entry(cdb, "/src/a.cpp", edits.options())) ==
            "clang++ -D BAR " + CanonicalPath(Spelling::absolute("/src/a.cpp")).str());
}

ZEST_CASE(RuleOrderLaterAppendWins) {
    // Later append comes after earlier append — at compiler level, last wins
    // for flags like -O; verify the ordering is preserved.
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-O2"},
    });
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-O3"},
    });
    config.finalize(CanonicalPath(Spelling::absolute("/src")));

    std::vector<std::string> append, remove;
    match_rules(config, "/src/a.cpp", append, remove);
    ZEXPECT(append.size() == 2u);
    ZEXPECT(append[0] == "-O2");
    ZEXPECT(append[1] == "-O3");
}

ZEST_CASE(InitOptionsOverlayPreservesToml) {
    // Mirror the master_server flow: load workspace config from clice.toml first,
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

    auto config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.root)));
    auto from_toml = CanonicalPath(Spelling("/from/toml", Spelling::absolute(tmp.root))).str();
    ZEXPECT(std::string_view(config.project.cache_dir) == from_toml);
    ZEXPECT(config.project.test_hooks.value == true);
    ZEXPECT(config.project.idle_timeout_ms.value == 16u);
    ZEXPECT(config.compiled_rules.size() == 1u);

    // Overlay only `idle_timeout_ms` via JSON.
    auto ov = kota::codec::json::from_string(R"({ "project": { "idle_timeout_ms": 99 } })", config);
    ZEXPECT(ov);
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

    // Overridden field.
    ZEXPECT(config.project.idle_timeout_ms.value == 99u);
    // Untouched fields stay at TOML values.
    ZEXPECT(std::string_view(config.project.cache_dir) == from_toml);
    ZEXPECT(config.project.test_hooks.value == true);
    // Rules from clice.toml must survive the overlay.
    ZEXPECT(config.rules.size() == 1u);
    ZEXPECT(config.compiled_rules.size() == 1u);
    ZEXPECT(config.rules[0].append[0] == "-DFROM_TOML");
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
    auto config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.root)),
                                              nullptr,
                                              nullptr,
                                              /*finalized=*/false);
    auto ov = kota::codec::json::from_string(
        R"({ "inlay_hints": { "parameters": false, "block_end": false }, "code_completion": { "limit": 5 } })",
        config);
    ZEXPECT(ov);
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

    // From the TOML layer.
    ZEXPECT(config.code_completion.bundle_overloads.value == false);
    // From the JSON overlay, including a nested field both layers set —
    // the later source wins.
    ZEXPECT(config.inlay_hints.block_end.value == false);
    ZEXPECT(config.inlay_hints.parameters.value == false);
    ZEXPECT(config.code_completion.limit.value == 5u);
    // Named by nobody: field-initializer defaults.
    ZEXPECT(config.inlay_hints.deduced_types.value == true);
    ZEXPECT(config.code_completion.insert_paren_in_function_call.value == false);
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
    auto config = Config::load_from_workspace(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(config.compiled_rules.size() == 1u);

    auto ov = kota::codec::json::from_string(
        R"({ "rules": [ { "patterns": ["**/*.cc"], "append": ["-DFROM_JSON"] } ] })",
        config);
    ZEXPECT(ov);
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));

    ZEXPECT(config.rules.size() == 1u);
    ZEXPECT(config.rules[0].append[0] == "-DFROM_JSON");
    ZEXPECT(config.compiled_rules.size() == 1u);

    // Original TOML rule no longer applies.
    std::vector<std::string> append, remove;
    match_rules(config, "/src/x.cpp", append, remove);
    ZEXPECT(append.empty());
    match_rules(config, "/src/x.cc", append, remove);
    ZEXPECT(append.size() == 1u);
    ZEXPECT(append[0] == "-DFROM_JSON");
}

ZEST_CASE(JsonSchema) {
    auto schema = Config::json_schema();
    ZASSERT(schema);
    auto doc = kota::codec::json::from_string<kota::codec::dyn::Value>(*schema);
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
    ZEXPECT(*modes == kota::codec::dyn::Value(kota::codec::dyn::Array{"off", "on", "auto"}));

    // Root and every section body reject unknown properties, so editors
    // flag typos the way loading reports unknown keys.
    auto denies_unknown = [](const kota::codec::dyn::Value& body) {
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

};  // ZEST_SUITE(Config)

}  // namespace clice::testing
