#include <cstdlib>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/config.h"

#include "kota/codec/json/json.h"
#include "kota/codec/toml.h"

namespace clice::testing {

// POSIX setenv/unsetenv don't exist on Windows; map to _putenv_s
// (passing an empty value to _putenv_s removes the variable).
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

static void unset_env(const char* name) {
#ifdef _WIN32
    ::_putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

TEST_SUITE(Config) {

TEST_CASE(ParsePartialProject) {
    auto result = kota::codec::toml::parse<ProjectConfig>(R"(cache_dir = "/tmp/test")");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->cache_dir), "/tmp/test");
    EXPECT_EQ(result->clang_tidy.value, false);
    EXPECT_EQ(result->max_active_file.value, 0);
    EXPECT_FALSE(result->enable_indexing.has_value());
    EXPECT_FALSE(result->idle_timeout_ms.has_value());
}

TEST_CASE(ParseConfigRule) {
    auto result = kota::codec::toml::parse<ConfigRule>(R"(
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
    auto result = kota::codec::toml::parse<Config>(R"(
[project]
cache_dir = "/tmp/test"
clang_tidy = true
enable_indexing = false

[[rules]]
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/tmp/test");
    EXPECT_EQ(result->project.clang_tidy.value, true);
    EXPECT_EQ(*result->project.enable_indexing, false);
    EXPECT_EQ(result->rules.size(), 1u);
    EXPECT_EQ(result->rules[0].patterns[0], "**/*.cpp");
}

TEST_CASE(ParseEmptyConfig) {
    auto result = kota::codec::toml::parse<Config>("");
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->rules.empty());
    EXPECT_TRUE(std::string_view(result->project.cache_dir).empty());
}

TEST_CASE(ParseOnlyRules) {
    auto result = kota::codec::toml::parse<Config>(R"(
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
    config.apply_defaults("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/foo.cpp", append, remove);
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
    config.apply_defaults("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/foo.h", append, remove);
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
    config.apply_defaults("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/test_foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 2u);
    EXPECT_EQ(append[0], "-DCPP");
    EXPECT_EQ(append[1], "-DTEST");
}

TEST_CASE(ApplyDefaults) {
    Config config;
    config.apply_defaults("/workspace");
    EXPECT_EQ(*config.project.enable_indexing, true);
    EXPECT_EQ(*config.project.idle_timeout_ms, 3000);
    EXPECT_EQ(config.project.max_active_file.value, 8);
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_EQ(config.project.stateless_worker_count.value, 3u);
    EXPECT_FALSE(config.project.cache_dir.empty());
    EXPECT_FALSE(config.project.index_dir.empty());
    EXPECT_FALSE(config.project.logging_dir.empty());
}

TEST_CASE(ApplyDefaultsEmptyWorkspace) {
    Config config;
    config.apply_defaults("");
    EXPECT_TRUE(config.project.cache_dir.empty());
    EXPECT_TRUE(config.project.index_dir.empty());
    EXPECT_TRUE(config.project.logging_dir.empty());
}

TEST_CASE(ApplyDefaultsPreserveSet) {
    Config config;
    config.project.cache_dir = "/custom";
    config.project.enable_indexing = false;
    config.apply_defaults("/workspace");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/custom");
    EXPECT_EQ(*config.project.enable_indexing, false);
}

TEST_CASE(LoadFromJson) {
    auto result = Config::load_from_json(R"({
        "project": {
            "cache_dir": "/opt/cache",
            "clang_tidy": true,
            "enable_indexing": false
        },
        "rules": [
            { "patterns": ["**/*.cpp"], "append": ["-DFOO"] }
        ]
    })",
                                         "/workspace");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/opt/cache");
    EXPECT_EQ(result->project.clang_tidy.value, true);
    EXPECT_EQ(*result->project.enable_indexing, false);
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

TEST_CASE(LoadMissingFile) {
    auto result = Config::load("/nonexistent/clice.toml", "/workspace");
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(WorkspaceVarSubst) {
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.project.index_dir = "${workspace}/idx";
    config.project.logging_dir = "${workspace}/logs";
    config.project.compile_commands_paths = {"${workspace}/build"};
    config.apply_defaults("/my/ws");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/my/ws/cache");
    EXPECT_EQ(std::string_view(config.project.index_dir), "/my/ws/idx");
    EXPECT_EQ(std::string_view(config.project.logging_dir), "/my/ws/logs");
    EXPECT_EQ(config.project.compile_commands_paths[0], "/my/ws/build");
}

TEST_CASE(XdgCacheDir) {
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.apply_defaults("/some/ws");
    unset_env("XDG_CACHE_HOME");

    std::string_view cache(config.project.cache_dir);
    EXPECT_TRUE(cache.starts_with(cache_base));
    EXPECT_TRUE(cache.find("/clice/") != std::string_view::npos);
}

TEST_CASE(InvalidGlobPattern) {
    Config config;
    // All-invalid patterns: rule must be dropped entirely, not appended as empty.
    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}"},
        .append = {"-DSHOULD_NOT_APPEAR"},
    });
    // Mixed valid/invalid: only the invalid pattern is skipped; rule remains.
    config.rules.push_back(ConfigRule{
        .patterns = {"**/****.{c,cc}", "**/*.cpp"},
        .append = {"-DCPP"},
    });
    config.apply_defaults("");
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    std::vector<std::string> append, remove;
    config.match_rules("/src/foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-DCPP");
}

TEST_CASE(ConfigPriorityJson) {
    // initializationOptions-sourced config should override an on-disk default.
    auto from_json =
        Config::load_from_json(R"({ "project": { "max_active_file": 42 } })", "/workspace");
    EXPECT_TRUE(from_json.has_value());
    EXPECT_EQ(from_json->project.max_active_file.value, 42);
    // Unset fields still receive defaults.
    EXPECT_EQ(*from_json->project.enable_indexing, true);
    EXPECT_EQ(from_json->project.stateful_worker_count.value, 2u);
}

TEST_CASE(XdgHashUnique) {
    // Different workspace roots must map to different cache dirs,
    // same workspace root must map to the same dir (deterministic).
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());

    Config a, b, c;
    a.apply_defaults("/ws/project-a");
    b.apply_defaults("/ws/project-b");
    c.apply_defaults("/ws/project-a");
    unset_env("XDG_CACHE_HOME");

    EXPECT_NE(std::string_view(a.project.cache_dir), std::string_view(b.project.cache_dir));
    EXPECT_EQ(std::string_view(a.project.cache_dir), std::string_view(c.project.cache_dir));
}

TEST_CASE(HomeFallback) {
    // With XDG_CACHE_HOME unset but HOME set, cache dir should be under $HOME/.cache/clice.
    TempDir tmp;
    unset_env("XDG_CACHE_HOME");
    auto home = tmp.path("home");
    // Save prior value so we restore cleanly.
    const char* prior = std::getenv("HOME");
    std::string prior_home = prior ? prior : "";
    set_env("HOME", home.c_str());

    Config config;
    config.apply_defaults("/some/ws");

    if(prior_home.empty())
        unset_env("HOME");
    else
        set_env("HOME", prior_home.c_str());

    std::string_view cache(config.project.cache_dir);
    EXPECT_TRUE(cache.starts_with(home + "/.cache/clice/"));
}

TEST_CASE(WorkspaceCacheFallback) {
    // No XDG, no HOME → should fall back to ${workspace}/.clice.
    unset_env("XDG_CACHE_HOME");
    const char* prior = std::getenv("HOME");
    std::string prior_home = prior ? prior : "";
    unset_env("HOME");

    Config config;
    config.apply_defaults("/ws/root");

    if(!prior_home.empty())
        set_env("HOME", prior_home.c_str());

    EXPECT_EQ(std::string_view(config.project.cache_dir), "/ws/root/.clice");
    EXPECT_EQ(std::string_view(config.project.index_dir), "/ws/root/.clice/index");
    EXPECT_EQ(std::string_view(config.project.logging_dir), "/ws/root/.clice/logs");
}

TEST_CASE(WorkspaceSubstEmpty) {
    // Empty workspace_root must not rewrite "${workspace}" into "" and produce
    // bogus paths like "/cache" — the placeholder should be left intact.
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.apply_defaults("");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "${workspace}/cache");
}

TEST_CASE(WorkspaceSubstRepeated) {
    // Multiple ${workspace} occurrences in one string all get substituted.
    Config config;
    config.project.cache_dir = "${workspace}/a/${workspace}/b";
    config.apply_defaults("/root");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/root/a//root/b");
}

TEST_CASE(CompilePathsList) {
    // compile_commands_paths should substitute ${workspace} on every entry.
    Config config;
    config.project.compile_commands_paths = {
        "${workspace}/build",
        "/abs/path/compile_commands.json",
        "${workspace}/out",
    };
    config.apply_defaults("/ws");
    EXPECT_EQ(config.project.compile_commands_paths.size(), 3u);
    EXPECT_EQ(config.project.compile_commands_paths[0], "/ws/build");
    EXPECT_EQ(config.project.compile_commands_paths[1], "/abs/path/compile_commands.json");
    EXPECT_EQ(config.project.compile_commands_paths[2], "/ws/out");
}

TEST_CASE(TomlErrorLocated) {
    // Malformed TOML (bad table header, missing close-bracket) must return nullopt.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\nclang_tidy = true\n");
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str());
    EXPECT_FALSE(result.has_value());
}

TEST_CASE(WorkspaceMalformedFallback) {
    // load_from_workspace must fall back to defaults when clice.toml is malformed,
    // not propagate the failure.
    TempDir tmp;
    tmp.touch("clice.toml", "[project\ninvalid");
    auto config = Config::load_from_workspace(tmp.root.str());
    // Defaults still applied.
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_EQ(*config.project.enable_indexing, true);
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
    config.apply_defaults("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/a.cpp", append, remove);

    // -DFOO should have been stripped from append; -DBAR remains.
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-DBAR");
    // remove is still forwarded so base CDB flags also get filtered.
    EXPECT_EQ(remove.size(), 1u);
    EXPECT_EQ(remove[0], "-DFOO");
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
    config.apply_defaults("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/a.cpp", append, remove);
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
clang_tidy = true
max_active_file = 16

[[rules]]
patterns = ["**/*.cpp"]
append = ["-DFROM_TOML"]
)");

    auto config = Config::load_from_workspace(tmp.root.str());
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/from/toml");
    EXPECT_EQ(config.project.clang_tidy.value, true);
    EXPECT_EQ(config.project.max_active_file.value, 16);
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    // Overlay only `max_active_file` via JSON.
    auto ov = kota::codec::json::parse(R"({ "project": { "max_active_file": 99 } })", config);
    EXPECT_TRUE(ov.has_value());
    config.apply_defaults(tmp.root.str());

    // Overridden field.
    EXPECT_EQ(config.project.max_active_file.value, 99);
    // Untouched fields stay at TOML values.
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/from/toml");
    EXPECT_EQ(config.project.clang_tidy.value, true);
    // Rules from clice.toml must survive the overlay.
    EXPECT_EQ(config.rules.size(), 1u);
    EXPECT_EQ(config.compiled_rules.size(), 1u);
    EXPECT_EQ(config.rules[0].append[0], "-DFROM_TOML");
}

TEST_CASE(InitOptionsOverlayRulesReplace) {
    // When `rules` is present in the overlay JSON, it replaces the whole array
    // (kotatsu deserializes the vector by value). `compiled_rules` must be
    // rebuilt after apply_defaults so stale compiled entries don't linger.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[[rules]]
patterns = ["**/*.cpp"]
append = ["-DTOML_ONLY"]
)");
    auto config = Config::load_from_workspace(tmp.root.str());
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    auto ov = kota::codec::json::parse(
        R"({ "rules": [ { "patterns": ["**/*.cc"], "append": ["-DFROM_JSON"] } ] })",
        config);
    EXPECT_TRUE(ov.has_value());
    config.apply_defaults(tmp.root.str());

    EXPECT_EQ(config.rules.size(), 1u);
    EXPECT_EQ(config.rules[0].append[0], "-DFROM_JSON");
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    // Original TOML rule no longer applies.
    std::vector<std::string> append, remove;
    config.match_rules("/src/x.cpp", append, remove);
    EXPECT_TRUE(append.empty());
    config.match_rules("/src/x.cc", append, remove);
    EXPECT_EQ(append.size(), 1u);
    EXPECT_EQ(append[0], "-DFROM_JSON");
}

};  // TEST_SUITE(Config)

}  // namespace clice::testing
