#include <format>

#include "test/cdb_helper.h"
#include "test/temp_dir.h"
#include "test/test.h"
#include "config/config.h"
#include "sched/build.h"
#include "support/filesystem.h"

#include "kota/codec/dyn/decode.h"
#include "kota/codec/json/json.h"
#include "kota/codec/toml/toml.h"

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
    for(auto& edit: build.edits(path).edits) {
        auto& out = edit.kind == CommandEdit::Kind::Remove ? remove : append;
        out.insert(out.end(), edit.flags.begin(), edit.flags.end());
    }
}

TEST_SUITE(Config) {

TEST_CASE(ParsePartialProject) {
    // A partial decode only touches the fields it names; everything else
    // keeps the field-initializer defaults.
    auto result = kota::codec::toml::from_string<ProjectConfig>(R"(cache_dir = "/tmp/test")");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->cache_dir), "/tmp/test");
    EXPECT_EQ(result->enable_indexing.value, true);
    EXPECT_EQ(result->idle_timeout_ms.value, 3000u);
}

TEST_CASE(ParseConfigRule) {
    auto result = kota::codec::toml::from_string<ConfigRule>(R"(
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->patterns.size(), 1u);
    EXPECT_EQ(result->patterns[0], "**/*.cpp");
    EXPECT_EQ(result->append[0], "-std=c++20");
    EXPECT_TRUE(result->remove.empty());
}

TEST_CASE(ParseFullConfig) {
    auto result = kota::codec::toml::from_string<Config>(R"(
[project]
cache_dir = "/tmp/test"
enable_indexing = false

[[rules]]
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/tmp/test");
    EXPECT_EQ(result->project.enable_indexing.value, false);
    EXPECT_EQ(result->rules.size(), 1u);
    EXPECT_EQ(result->rules[0].patterns[0], "**/*.cpp");
}

TEST_CASE(ParseInlayHints) {
    auto result = kota::codec::toml::from_string<Config>(R"(
[inlay_hints]
block_end = true
parameters = false
type_name_limit = 64
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->inlay_hints.block_end.value, true);
    EXPECT_EQ(result->inlay_hints.parameters.value, false);
    EXPECT_EQ(result->inlay_hints.type_name_limit.value, 64u);
    EXPECT_EQ(result->inlay_hints.designators.value, true);
}

TEST_CASE(ParseEmptyConfig) {
    auto result = kota::codec::toml::from_string<Config>("");
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->rules.empty());
    EXPECT_TRUE(std::string_view(result->project.cache_dir).empty());
}

TEST_CASE(ParseOnlyRules) {
    auto result = kota::codec::toml::from_string<Config>(R"(
[[rules]]
patterns = ["*.h"]
remove = ["-Werror"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->rules.size(), 1u);
    EXPECT_EQ(result->rules[0].patterns[0], "*.h");
    EXPECT_EQ(result->rules[0].remove[0], "-Werror");
    EXPECT_TRUE(std::string_view(result->project.cache_dir).empty());
}

TEST_CASE(MatchRulesBasic) {
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-std=c++20"},
        .remove = {"-std=c++17"},
    });
    config.finalize("");

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-std=c++20");
    EXPECT_EQ(remove.size(), 1u);
    EXPECT_EQ(remove[0], "-std=c++17");
}

TEST_CASE(MatchRulesNoMatch) {
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DFOO"},
    });
    config.finalize("");

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.h", append, remove);
    EXPECT_TRUE(append.empty());
    EXPECT_TRUE(remove.empty());
}

TEST_CASE(MatchRulesMultiple) {
    Config config;
    config.rules.push_back(ConfigRule{
        .patterns = {"**/*.cpp"},
        .append = {"-DCPP"},
    });
    config.rules.push_back(ConfigRule{
        .patterns = {"**/test_*.cpp"},
        .append = {"-DTEST"},
    });
    config.finalize("");

    std::vector<std::string> append, remove;
    match_rules(config, "/src/test_foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 2u);
    EXPECT_EQ(append[0], "-DCPP");
    EXPECT_EQ(append[1], "-DTEST");
}

TEST_CASE(BornValidDefaults) {
    // A default-constructed Config is fully valid without any init step;
    // the option defaults come from the field initializers alone.
    Config config;
    EXPECT_EQ(config.project.enable_indexing.value, true);
    EXPECT_EQ(config.project.idle_timeout_ms.value, 3000u);
    EXPECT_EQ(config.project.test_hooks.value, false);
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_GE(config.project.stateless_worker_count.value, 2u);
    EXPECT_EQ(config.project.min_stateless_worker_count.value, 1u);
    EXPECT_EQ(config.project.max_stateless_worker_count.value,
              default_max_stateless_worker_count());
    EXPECT_GE(config.project.max_stateless_worker_count.value,
              config.project.min_stateless_worker_count.value);
    EXPECT_EQ(config.tracker.cdb_poll_seconds.value, 3u);
    EXPECT_EQ(config.tracker.workspace_poll_seconds.value, 30u);
    EXPECT_EQ(config.inlay_hints.enabled.value, true);
    EXPECT_EQ(config.inlay_hints.parameters.value, true);
    EXPECT_EQ(config.inlay_hints.deduced_types.value, true);
    EXPECT_EQ(config.inlay_hints.designators.value, true);
    EXPECT_EQ(config.inlay_hints.block_end.value, false);
    EXPECT_EQ(config.inlay_hints.default_arguments.value, false);
    EXPECT_EQ(config.inlay_hints.type_name_limit.value, 32u);
    EXPECT_EQ(config.code_completion.enable_keyword_snippet.value, false);
    EXPECT_EQ(config.code_completion.enable_function_arguments_snippet.value, false);
    EXPECT_EQ(config.code_completion.enable_template_arguments_snippet.value, false);
    EXPECT_EQ(config.code_completion.insert_paren_in_function_call.value, false);
    EXPECT_EQ(config.code_completion.bundle_overloads.value, true);
    EXPECT_EQ(config.code_completion.limit.value, 0u);
}

TEST_CASE(FinalizeDerivesPaths) {
    Config config;
    config.finalize("/workspace");
    EXPECT_FALSE(config.project.cache_dir.empty());
    EXPECT_FALSE(config.project.logging_dir.empty());
    EXPECT_EQ(config.project.cache_dir_defaulted.value, true);
}

TEST_CASE(FinalizeEmptyWorkspace) {
    Config config;
    config.finalize("");
    EXPECT_TRUE(config.project.cache_dir.empty());
    EXPECT_TRUE(config.project.logging_dir.empty());
}

TEST_CASE(FinalizePreservesSet) {
    Config config;
    config.project.cache_dir = "/custom";
    config.project.enable_indexing = false;
    config.inlay_hints.parameters = false;
    config.inlay_hints.block_end = true;
    config.finalize("/workspace");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/custom");
    EXPECT_EQ(config.project.cache_dir_defaulted.value, false);
    EXPECT_EQ(config.project.enable_indexing.value, false);
    EXPECT_EQ(config.inlay_hints.parameters.value, false);
    EXPECT_EQ(config.inlay_hints.block_end.value, true);
}

TEST_CASE(LoadFromJson) {
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
                                         "/workspace");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/opt/cache");
    EXPECT_EQ(result->project.enable_indexing.value, false);
    EXPECT_EQ(result->rules.size(), 1u);
    EXPECT_EQ(result->compiled_rules.size(), 1u);
}

TEST_CASE(LoadFromJsonInvalid) {
    auto result = Config::load_from_json("{not valid json", "/workspace");
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(LoadMalformedToml) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\nbroken");
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str().str());
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(LegacyProjectKeysIgnored) {
    // Configs written for older clice may still set removed project keys;
    // unknown keys must not fail the parse.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[project]
cache_dir = "/opt/cache"
index_dir = "/opt/index"
worker_memory_limit = 4294967296
)");
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str().str());
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/opt/cache");
}

TEST_CASE(LoadMissingFile) {
    auto result = Config::load("/nonexistent/clice.toml", "/workspace");
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(WorkspaceVarSubst) {
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
    config.finalize(tmp.root.str());
    EXPECT_EQ(std::string_view(config.project.cache_dir), at("cache"));
    EXPECT_EQ(std::string_view(config.project.logging_dir), at("logs"));
    ASSERT_EQ(config.compiled_rules.size(), 1u);
    EXPECT_EQ(config.compiled_rules[0].compile_commands[0], at("build"));
}

TEST_CASE(ParseRuleSources) {
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
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->default_configuration), "debug");
    ASSERT_EQ(result->rules.size(), 2u);
    EXPECT_EQ(std::string_view(result->rules[0].configuration), "debug");
    EXPECT_EQ(result->rules[0].compile_commands.size(), 2u);
    EXPECT_EQ(std::get<std::string>(result->rules[0].default_command),
              "clang++ -std=c++20 -Iinclude");
    EXPECT_TRUE(result->rules[0].index);
    EXPECT_EQ(std::get<std::vector<std::string>>(result->rules[1].default_command).size(), 2u);
    EXPECT_FALSE(result->rules[1].index);
}

TEST_CASE(AnchoredRules) {
    /// Relative databases and patterns anchor at the configuration file's
    /// directory, `..` included; `**`-led and absolute patterns match
    /// absolute paths; the top-level databases become the trailing rule.
    TempDir tmp;
    auto at = [&](llvm::StringRef relative) {
        std::string p = tmp.path(relative);
        path::canonicalize(p);
        return p;
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

    auto loaded = Config::load(tmp.path("sub/clice.toml"), root);
    ASSERT_TRUE(loaded.has_value());
    auto& config = *loaded;

    ASSERT_EQ(config.compiled_rules.size(), 3u);
    EXPECT_EQ(config.compiled_rules[0].compile_commands[0], at("sub/out/debug"));
    EXPECT_EQ(config.compiled_rules[0].directory, at("sub"));
    EXPECT_EQ(config.compiled_rules[0].patterns[0].root, at("sub/src"));
    EXPECT_TRUE(config.compiled_rules[0].declares_sources());
    /// The string spelling is tokenized, then `${workspace}` substituted
    /// per argument.
    ASSERT_EQ(config.compiled_rules[0].default_command.size(), 3u);
    EXPECT_EQ(config.compiled_rules[0].default_command[2], "-I" + at("include"));
    EXPECT_FALSE(config.compiled_rules[1].declares_sources());
    ASSERT_EQ(config.compiled_rules[1].patterns.size(), 4u);
    EXPECT_EQ(config.compiled_rules[1].patterns[0].root, root);
    EXPECT_EQ(config.compiled_rules[1].patterns[1].root, at("gen"));
    EXPECT_EQ(config.compiled_rules[1].patterns[2].root, at("shared"));
    EXPECT_EQ(config.compiled_rules[1].patterns[3].root, at("sub"));
    EXPECT_TRUE(config.compiled_rules[2].patterns.empty());
    EXPECT_EQ(config.compiled_rules[2].compile_commands[0], at("sub/build"));
    EXPECT_EQ(config.compiled_rules[2].compile_commands[1], at("elsewhere/compile_commands.json"));
    EXPECT_TRUE(config.declares_sources());

    auto tags = config.configurations();
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0], "debug");

    /// A relative pattern sees only files under its anchor; a bare `*`
    /// names the anchor's direct children.
    EXPECT_EQ(config.matching_rules(at("sub/src/a.cpp"), "debug").size(), 2u);
    EXPECT_EQ(config.matching_rules(at("sub/src/a.cpp"), "release").size(), 1u);
    EXPECT_EQ(config.matching_rules(at("src/a.cpp"), "debug").size(), 1u);
    EXPECT_EQ(config.matching_rules(at("sub/a.cpp"), "debug").size(), 2u);
    /// `**` and `${workspace}` patterns match the absolute path; `..`
    /// climbs out of the anchor, `*` stays within one segment.
    auto hxx = config.matching_rules(at("other/tree/x.hxx"), "debug");
    ASSERT_EQ(hxx.size(), 2u);
    EXPECT_EQ(hxx[0]->append.size(), 2u);
    EXPECT_EQ(config.matching_rules(at("gen/x.cpp"), "debug").size(), 2u);
    EXPECT_EQ(config.matching_rules(at("shared/x.cpp"), "debug").size(), 2u);
    EXPECT_EQ(config.matching_rules(at("shared/deep/x.cpp"), "debug").size(), 1u);
}

TEST_CASE(InitOptionsAnchorAtWorkspace) {
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

    auto from_file = Config::load_from_workspace(root);
    ASSERT_EQ(from_file.compiled_rules.size(), 2u);
    EXPECT_EQ(from_file.compiled_rules[0].patterns[0].root, at("src"));
    EXPECT_EQ(from_file.compiled_rules[0].directory, at(".clice"));
    EXPECT_EQ(from_file.compiled_rules[1].compile_commands[0], at("build"));

    auto config = Config::load_from_workspace(root, nullptr, nullptr, /*finalized=*/false);
    auto ov = kota::codec::json::from_string(
        R"({ "rules": [{ "patterns": ["src/**"], "compile_commands": ["cmake"] }, { "compile_commands": ["out"] }] })",
        config);
    ASSERT_TRUE(ov.has_value());
    config.finalize(root);

    ASSERT_EQ(config.compiled_rules.size(), 2u);
    EXPECT_EQ(config.compiled_rules[0].patterns[0].root, at("src"));
    EXPECT_EQ(config.compiled_rules[0].compile_commands[0], at("cmake"));
    EXPECT_EQ(config.compiled_rules[0].directory, root);
    EXPECT_EQ(config.compiled_rules[1].compile_commands[0], at("out"));
}

TEST_CASE(SourcesOffByDefault) {
    Config config;
    config.rules.push_back(ConfigRule{.patterns = {"**/*"}, .append = {"-DX"}});
    config.finalize("/ws");
    EXPECT_FALSE(config.declares_sources());
    EXPECT_TRUE(config.configurations().empty());
}

TEST_CASE(InvalidGlobPattern) {
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
    config.finalize("");
    ASSERT_EQ(config.compiled_rules.size(), 2u);
    EXPECT_TRUE(config.compiled_rules[0].unmatchable);
    EXPECT_TRUE(config.compiled_rules[0].declares_sources());
    EXPECT_FALSE(config.compiled_rules[1].unmatchable);

    std::vector<std::string> append, remove;
    match_rules(config, "/src/foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-DCPP");
}

TEST_CASE(ConfigPriorityJson) {
    // initializationOptions-sourced config should override an on-disk default.
    auto from_json =
        Config::load_from_json(R"({ "project": { "idle_timeout_ms": 42 } })", "/workspace");
    EXPECT_TRUE(from_json.has_value());
    EXPECT_EQ(from_json->project.idle_timeout_ms.value, 42u);
    // Unset fields still receive defaults.
    EXPECT_EQ(from_json->project.enable_indexing.value, true);
    EXPECT_EQ(from_json->project.stateful_worker_count.value, 2u);
}

TEST_CASE(DefaultWorkspaceCache) {
    Config config;
    config.finalize("/ws/root");

    EXPECT_EQ(path::convert_to_slash(std::string_view(config.project.cache_dir)),
              "/ws/root/.clice");
    EXPECT_EQ(path::convert_to_slash(std::string_view(config.project.logging_dir)),
              "/ws/root/.clice/logs");
}

TEST_CASE(WorkspaceSubstEmpty) {
    // Empty workspace_root must not rewrite "${workspace}" into "" and produce
    // bogus paths like "/cache" — the placeholder should be left intact.
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.finalize("");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "${workspace}/cache");
}

TEST_CASE(WorkspaceSubstRepeated) {
    // Multiple ${workspace} occurrences in one string all get substituted.
    Config config;
    config.project.cache_dir = "${workspace}/a/${workspace}/b";
    config.finalize("/root");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/root/a//root/b");
}

TEST_CASE(CompileCommandsList) {
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
    config.finalize(tmp.root.str());
    ASSERT_EQ(config.compiled_rules.size(), 1u);
    auto& databases = config.compiled_rules[0].compile_commands;
    ASSERT_EQ(databases.size(), 4u);
    EXPECT_EQ(databases[0], at("build"));
    EXPECT_EQ(databases[1], at("abs/path/compile_commands.json"));
    EXPECT_EQ(databases[2], at("out"));
    EXPECT_EQ(databases[3], at("build.json/compile_commands.json"));
}

TEST_CASE(TomlErrorLocated) {
    // Malformed TOML (bad table header, missing close-bracket) must return nullopt.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str());
    EXPECT_FALSE(result.has_value());
}

// FIXME: assert ConfigIssue::line/column once kotatsu's TOML decoder exposes
// error locations (feature 60661c1 on the unmerged kotatsu branch); the
// plumbing here already forwards rich_error.location when present.
TEST_CASE(SyntaxIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ntest_hooks = true\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_FALSE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Error);
}

TEST_CASE(TypeIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\ntest_hooks = \"yes\"\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_FALSE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Error);
    EXPECT_NE(issues[0].message.find("test_hooks"), std::string::npos);
}

TEST_CASE(RemovedKeyIssueWarns) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nworker_memory_limit = 4294967296\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Warning);
    EXPECT_NE(issues[0].message.find("worker_memory_limit"), std::string::npos);
}

TEST_CASE(UnknownFeatureKeyWarns) {
    // Feature options structs double as config sections; a typo inside one
    // must still surface as a Warning, not vanish.
    TempDir tmp;
    tmp.touch("clice.toml", "[inlay_hints]\nblokc_end = true\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Warning);
    EXPECT_NE(issues[0].message.find("blokc_end"), std::string::npos);
}

TEST_CASE(ZeroWorkerCountRejected) {
    // An explicit 0 is not a runnable worker configuration; finalize
    // validates it back to the born-valid default instead of starting a
    // pool with no workers.
    Config config;
    config.project.stateful_worker_count = 0;
    config.project.stateless_worker_count = 0;
    config.project.min_stateless_worker_count = 0;
    config.finalize("");
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_GE(config.project.stateless_worker_count.value, 2u);
    EXPECT_EQ(config.project.min_stateless_worker_count.value, 1u);
}

TEST_CASE(NullOptionRejected) {
    // `defaulted` means "may be absent", never "may be null": an explicit
    // JSON null on an option is a type error and fails the whole decode,
    // both flat and inside a feature section.
    auto flat = Config::load_from_json(R"({ "project": { "test_hooks": null } })", "/ws");
    EXPECT_FALSE(flat.has_value());
    auto nested = Config::load_from_json(R"({ "inlay_hints": { "block_end": null } })", "/ws");
    EXPECT_FALSE(nested.has_value());
}

TEST_CASE(WorkspaceMalformedFallback) {
    // load_from_workspace must fall back to defaults when clice.toml is malformed,
    // not propagate the failure.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ninvalid");
    auto config = Config::load_from_workspace(tmp.root.str());
    // Defaults still applied.
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_EQ(config.project.enable_indexing.value, true);
}

TEST_CASE(RuleOrderLaterRemoveWins) {
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
    config.finalize("");

    // The edits stay in rule order; the remove takes effect against the
    // earlier append when the command is built, and against the base.
    FileTable files;
    CompilationDatabase cdb{files};
    cdb.add_command("/src", "/src/a.cpp", std::string_view("clang++ -DFOO a.cpp"));
    Build build{config, cdb, files};
    auto edits = build.edits(llvm::StringRef("/src/a.cpp"));
    ASSERT_EQ(edits.edits.size(), 2u);
    EXPECT_EQ(edits.edits[1].kind, CommandEdit::Kind::Remove);
    EXPECT_EQ(print_argv(render_entry(cdb, "/src/a.cpp", edits.options())),
              "clang++ -D BAR /src/a.cpp");
}

TEST_CASE(RuleOrderLaterAppendWins) {
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
    config.finalize("");

    std::vector<std::string> append, remove;
    match_rules(config, "/src/a.cpp", append, remove);
    EXPECT_EQ(append.size(), 2u);
    EXPECT_EQ(append[0], "-O2");
    EXPECT_EQ(append[1], "-O3");
}

TEST_CASE(InitOptionsOverlayPreservesToml) {
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

    auto config = Config::load_from_workspace(tmp.root.str());
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/from/toml");
    EXPECT_EQ(config.project.test_hooks.value, true);
    EXPECT_EQ(config.project.idle_timeout_ms.value, 16u);
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    // Overlay only `idle_timeout_ms` via JSON.
    auto ov = kota::codec::json::from_string(R"({ "project": { "idle_timeout_ms": 99 } })", config);
    EXPECT_TRUE(ov.has_value());
    config.finalize(tmp.root.str());

    // Overridden field.
    EXPECT_EQ(config.project.idle_timeout_ms.value, 99u);
    // Untouched fields stay at TOML values.
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/from/toml");
    EXPECT_EQ(config.project.test_hooks.value, true);
    // Rules from clice.toml must survive the overlay.
    EXPECT_EQ(config.rules.size(), 1u);
    EXPECT_EQ(config.compiled_rules.size(), 1u);
    EXPECT_EQ(config.rules[0].append[0], "-DFROM_TOML");
}

TEST_CASE(OverlaySectionDeepMerge) {
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
    auto config = Config::load_from_workspace(tmp.root.str(),
                                              nullptr,
                                              nullptr,
                                              /*finalized=*/false);
    auto ov = kota::codec::json::from_string(
        R"({ "inlay_hints": { "parameters": false, "block_end": false }, "code_completion": { "limit": 5 } })",
        config);
    EXPECT_TRUE(ov.has_value());
    config.finalize(tmp.root.str());

    // From the TOML layer.
    EXPECT_EQ(config.code_completion.bundle_overloads.value, false);
    // From the JSON overlay, including a nested field both layers set —
    // the later source wins.
    EXPECT_EQ(config.inlay_hints.block_end.value, false);
    EXPECT_EQ(config.inlay_hints.parameters.value, false);
    EXPECT_EQ(config.code_completion.limit.value, 5u);
    // Named by nobody: field-initializer defaults.
    EXPECT_EQ(config.inlay_hints.deduced_types.value, true);
    EXPECT_EQ(config.code_completion.insert_paren_in_function_call.value, false);
}

TEST_CASE(InitOptionsOverlayRulesReplace) {
    // When `rules` is present in the overlay JSON, it replaces the whole array
    // (kotatsu deserializes the vector by value). `compiled_rules` must be
    // rebuilt after finalize so stale compiled entries don't linger.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[[rules]]
patterns = ["**/*.cpp"]
append = ["-DTOML_ONLY"]
)");
    auto config = Config::load_from_workspace(tmp.root.str());
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    auto ov = kota::codec::json::from_string(
        R"({ "rules": [ { "patterns": ["**/*.cc"], "append": ["-DFROM_JSON"] } ] })",
        config);
    EXPECT_TRUE(ov.has_value());
    config.finalize(tmp.root.str());

    EXPECT_EQ(config.rules.size(), 1u);
    EXPECT_EQ(config.rules[0].append[0], "-DFROM_JSON");
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    // Original TOML rule no longer applies.
    std::vector<std::string> append, remove;
    match_rules(config, "/src/x.cpp", append, remove);
    EXPECT_TRUE(append.empty());
    match_rules(config, "/src/x.cc", append, remove);
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-DFROM_JSON");
}

TEST_CASE(JsonSchema) {
    auto schema = Config::json_schema();
    ASSERT_TRUE(schema.has_value());
    auto doc = kota::codec::json::from_string<kota::codec::dyn::Value>(*schema);
    ASSERT_TRUE(doc.has_value());

    // Machine-derived worker counts describe their derivation but carry no
    // default value anywhere — neither in their own schema nor inside a
    // section's whole-object default — so the schema stays byte-identical
    // across hosts.
    for(auto field: {"stateless_worker_count", "max_stateless_worker_count"}) {
        const auto* worker = find_property(*doc, field);
        ASSERT_TRUE(worker != nullptr);
        const auto* object = worker->get_object();
        ASSERT_TRUE(object != nullptr);
        EXPECT_TRUE(object->find("description") != nullptr);
        EXPECT_TRUE(object->find("default") == nullptr);
        EXPECT_TRUE(!default_mentions(*doc, field, false));
    }
    // Control: a stable field does appear under the section default.
    EXPECT_TRUE(default_mentions(*doc, "idle_timeout_ms", false));

    // A stable field initializer survives as the schema default, and the
    // unsigned field type keeps negative delays out of the schema.
    const auto* idle = find_property(*doc, "idle_timeout_ms");
    ASSERT_TRUE(idle != nullptr);
    EXPECT_TRUE(idle->get_object()->find("default") != nullptr);
    const auto* idle_minimum = idle->get_object()->find("minimum");
    ASSERT_TRUE(idle_minimum != nullptr);
    EXPECT_EQ(idle_minimum->get_uint().value_or(1), 0u);

    // skip = true fields stay out of the schema entirely.
    EXPECT_TRUE(find_property(*doc, "compiled_rules") == nullptr);

    // The fields finalize() rejects `0` for carry the matching lower bound.
    for(auto field:
        {"stateful_worker_count", "stateless_worker_count", "min_stateless_worker_count"}) {
        const auto* property = find_property(*doc, field);
        ASSERT_TRUE(property != nullptr);
        const auto* minimum = property->get_object()->find("minimum");
        ASSERT_TRUE(minimum != nullptr);
        EXPECT_EQ(minimum->get_uint().value_or(0), 1u);
    }

    // Enum fields name their accepted values, so editors flag a typo that
    // the lenient decode would silently turn into the default.
    const auto* readonly = find_property(*doc, "readonly");
    ASSERT_TRUE(readonly != nullptr);
    const auto* modes = readonly->get_object()->find("enum");
    ASSERT_TRUE(modes != nullptr);
    EXPECT_EQ(*modes, kota::codec::dyn::Value(kota::codec::dyn::Array{"off", "on", "auto"}));

    // Root and every section body reject unknown properties, so editors
    // flag typos the way the strict decode pass does.
    auto denies_unknown = [](const kota::codec::dyn::Value& body) {
        const auto* additional = body.get_object()->find("additionalProperties");
        return additional != nullptr && additional->get_bool() == false;
    };
    EXPECT_TRUE(denies_unknown(*doc));
    const auto* defs = doc->get_object()->find("$defs");
    ASSERT_TRUE(defs != nullptr);
    for(const auto& [name, body]: *defs->get_object()) {
        EXPECT_TRUE(denies_unknown(body));
    }
}

};  // TEST_SUITE(Config)

}  // namespace clice::testing
