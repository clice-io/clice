#include <cstdlib>

#include "test/temp_dir.h"
#include "test/test.h"
#include "server/state/config.h"
#include "support/filesystem.h"

#include "kota/codec/dyn/decode.h"
#include "kota/codec/json/json.h"
#include "kota/codec/toml/toml.h"

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

TEST_SUITE(Config) {

TEST_CASE(ParsePartialProject) {
    // A partial decode only touches the fields it names; everything else
    // keeps the field-initializer defaults.
    auto result = kota::codec::toml::from_string<ProjectConfig>(R"(cache_dir = "/tmp/test")");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->cache_dir), "/tmp/test");
    EXPECT_EQ(result->clang_tidy.value, false);
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
clang_tidy = true
enable_indexing = false

[[rules]]
patterns = ["**/*.cpp"]
append = ["-std=c++20"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(std::string_view(result->project.cache_dir), "/tmp/test");
    EXPECT_EQ(result->project.clang_tidy.value, true);
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
    config.finalize("");

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
    config.finalize("");

    std::vector<std::string> append, remove;
    config.match_rules("/src/test_foo.cpp", append, remove);
    EXPECT_EQ(append.size(), 2u);
    EXPECT_EQ(append[0], "-DCPP");
    EXPECT_EQ(append[1], "-DTEST");
}

TEST_CASE(BornValidDefaults) {
    // A default-constructed Config is fully valid without any init step;
    // the option defaults come from the field initializers alone.
    Config config;
    EXPECT_EQ(config.project.clang_tidy.value, false);
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
    EXPECT_EQ(config.project.worker_memory_limit.value, 4ULL * 1024 * 1024 * 1024);
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
    EXPECT_EQ(config.project.enable_indexing.value, false);
    EXPECT_EQ(config.inlay_hints.parameters.value, false);
    EXPECT_EQ(config.inlay_hints.block_end.value, true);
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

TEST_CASE(LegacyIndexDirIgnored) {
    // Configs written for older clice may still set the removed
    // project.index_dir key; unknown keys must not fail the parse.
    TempDir tmp;
    tmp.touch("clice.toml", R"(
[project]
cache_dir = "/opt/cache"
index_dir = "/opt/index"
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
    Config config;
    config.project.cache_dir = "${workspace}/cache";
    config.project.logging_dir = "${workspace}/logs";
    config.project.compile_commands_paths = {"${workspace}/build"};
    config.finalize("/my/ws");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/my/ws/cache");
    EXPECT_EQ(std::string_view(config.project.logging_dir), "/my/ws/logs");
    EXPECT_EQ(config.project.compile_commands_paths[0], "/my/ws/build");
}

TEST_CASE(XdgCacheDir) {
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.finalize("/some/ws");
    unset_env("XDG_CACHE_HOME");

    // Compare in the canonical spelling — finalize canonicalizes
    // cache_dir (a no-op on POSIX).
    std::string cache = path::convert_to_slash(std::string_view(config.project.cache_dir));
    std::string base = path::convert_to_slash(cache_base);
    path::canonicalize(base);
    EXPECT_TRUE(llvm::StringRef(cache).starts_with(base));
    EXPECT_TRUE(cache.find("/clice/") != std::string::npos);
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
    config.finalize("");
    EXPECT_EQ(config.compiled_rules.size(), 1u);

    std::vector<std::string> append, remove;
    config.match_rules("/src/foo.cpp", append, remove);
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

TEST_CASE(XdgHashUnique) {
    // Different workspace roots must map to different cache dirs,
    // same workspace root must map to the same dir (deterministic).
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());

    Config a, b, c;
    a.finalize("/ws/project-a");
    b.finalize("/ws/project-b");
    c.finalize("/ws/project-a");
    unset_env("XDG_CACHE_HOME");

    EXPECT_NE(std::string_view(a.project.cache_dir), std::string_view(b.project.cache_dir));
    EXPECT_EQ(std::string_view(a.project.cache_dir), std::string_view(c.project.cache_dir));
}

TEST_CASE(XdgNameHashFormat) {
    // The cache leaf is "<basename>-<8 hex digits>" so users can map
    // directories back to their workspaces.
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.finalize("/some/ws/myproject");
    unset_env("XDG_CACHE_HOME");

    llvm::StringRef leaf = path::filename(std::string_view(config.project.cache_dir));
    EXPECT_TRUE(leaf.starts_with("myproject-"));
    llvm::StringRef hex = leaf.drop_front(std::string_view("myproject-").size());
    EXPECT_EQ(hex.size(), 8u);
    EXPECT_EQ(hex.find_first_not_of("0123456789abcdef"), llvm::StringRef::npos);
}

TEST_CASE(XdgSameNameDiffer) {
    // Same basename under different parents: the hash must keep them apart.
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config a, b;
    a.finalize("/first/proj");
    b.finalize("/second/proj");
    unset_env("XDG_CACHE_HOME");

    EXPECT_TRUE(path::filename(std::string_view(a.project.cache_dir)).starts_with("proj-"));
    EXPECT_TRUE(path::filename(std::string_view(b.project.cache_dir)).starts_with("proj-"));
    EXPECT_NE(std::string_view(a.project.cache_dir), std::string_view(b.project.cache_dir));
}

TEST_CASE(XdgRootWorkspace) {
    // A root workspace has no basename; the leaf must not degenerate to "-<hash>".
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.finalize("/");
    unset_env("XDG_CACHE_HOME");

    EXPECT_TRUE(
        path::filename(std::string_view(config.project.cache_dir)).starts_with("workspace-"));
}

TEST_CASE(XdgLongBasename) {
    // A basename near the filesystem's 255-byte component limit must be
    // truncated so the leaf (name + "-" + 8 hex) still fits.
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.finalize("/ws/" + std::string(200, 'x'));
    unset_env("XDG_CACHE_HOME");

    llvm::StringRef leaf = path::filename(std::string_view(config.project.cache_dir));
    EXPECT_TRUE(leaf.starts_with(std::string(64, 'x')));
    EXPECT_EQ(leaf.size(), 64u + 9u);
}

TEST_CASE(XdgTrailingSlash) {
    TempDir tmp;
    auto cache_base = tmp.path("xdg");
    set_env("XDG_CACHE_HOME", cache_base.c_str());
    Config config;
    config.finalize("/some/ws/");
    unset_env("XDG_CACHE_HOME");

    EXPECT_TRUE(path::filename(std::string_view(config.project.cache_dir)).starts_with("ws-"));
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
    config.finalize("/some/ws");

    if(prior_home.empty())
        unset_env("HOME");
    else
        set_env("HOME", prior_home.c_str());

    std::string cache = path::convert_to_slash(std::string_view(config.project.cache_dir));
    std::string home_posix = path::convert_to_slash(home);
    path::canonicalize(home_posix);
    EXPECT_TRUE(llvm::StringRef(cache).starts_with(home_posix + "/.cache/clice/"));
}

TEST_CASE(WorkspaceCacheFallback) {
    // No XDG, no HOME → should fall back to ${workspace}/.clice.
    unset_env("XDG_CACHE_HOME");
    const char* prior = std::getenv("HOME");
    std::string prior_home = prior ? prior : "";
    unset_env("HOME");

    Config config;
    config.finalize("/ws/root");

    if(!prior_home.empty())
        set_env("HOME", prior_home.c_str());

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

TEST_CASE(CompilePathsList) {
    // compile_commands_paths should substitute ${workspace} on every entry.
    Config config;
    config.project.compile_commands_paths = {
        "${workspace}/build",
        "/abs/path/compile_commands.json",
        "${workspace}/out",
    };
    config.finalize("/ws");
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

// FIXME: assert ConfigIssue::line/column once kotatsu's TOML decoder exposes
// error locations (feature 60661c1 on the unmerged kotatsu branch); the
// plumbing here already forwards rich_error.location when present.
TEST_CASE(SyntaxIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project\nclang_tidy = true\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_FALSE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Error);
}

TEST_CASE(TypeIssueReported) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nclang_tidy = \"yes\"\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_FALSE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Error);
    EXPECT_NE(issues[0].message.find("clang_tidy"), std::string::npos);
}

TEST_CASE(UnknownKeyIssueWarns) {
    TempDir tmp;
    tmp.touch("clice.toml", "[project]\nclang_tdy = true\n");
    std::vector<ConfigIssue> issues;
    auto result = Config::load(tmp.path("clice.toml"), tmp.root.str(), &issues);
    EXPECT_TRUE(result.has_value());
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_EQ(issues[0].severity, ConfigIssue::Severity::Warning);
    EXPECT_NE(issues[0].message.find("clang_tdy"), std::string::npos);
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
    config.project.worker_memory_limit = 0;
    config.finalize("");
    EXPECT_EQ(config.project.stateful_worker_count.value, 2u);
    EXPECT_GE(config.project.stateless_worker_count.value, 2u);
    EXPECT_EQ(config.project.min_stateless_worker_count.value, 1u);
    EXPECT_EQ(config.project.worker_memory_limit.value, 4ULL * 1024 * 1024 * 1024);
}

TEST_CASE(NullOptionRejected) {
    // `defaulted` means "may be absent", never "may be null": an explicit
    // JSON null on an option is a type error and fails the whole decode,
    // both flat and inside a feature section.
    auto flat = Config::load_from_json(R"({ "project": { "clang_tidy": null } })", "/ws");
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
    config.finalize("");

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
idle_timeout_ms = 16

[[rules]]
patterns = ["**/*.cpp"]
append = ["-DFROM_TOML"]
)");

    auto config = Config::load_from_workspace(tmp.root.str());
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/from/toml");
    EXPECT_EQ(config.project.clang_tidy.value, true);
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
    EXPECT_EQ(config.project.clang_tidy.value, true);
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
    config.match_rules("/src/x.cpp", append, remove);
    EXPECT_TRUE(append.empty());
    config.match_rules("/src/x.cc", append, remove);
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
    for(auto field: {"stateful_worker_count",
                     "stateless_worker_count",
                     "min_stateless_worker_count",
                     "worker_memory_limit"}) {
        const auto* property = find_property(*doc, field);
        ASSERT_TRUE(property != nullptr);
        const auto* minimum = property->get_object()->find("minimum");
        ASSERT_TRUE(minimum != nullptr);
        EXPECT_EQ(minimum->get_uint().value_or(0), 1u);
    }

    // index_db names the accepted backends, so editors flag a typo that
    // open_database() would silently turn into LMDB.
    const auto* index_db = find_property(*doc, "index_db");
    ASSERT_TRUE(index_db != nullptr);
    const auto* backends = index_db->get_object()->find("enum");
    ASSERT_TRUE(backends != nullptr);
    EXPECT_EQ(*backends, kota::codec::dyn::Value(kota::codec::dyn::Array{"lmdb", "files"}));

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
