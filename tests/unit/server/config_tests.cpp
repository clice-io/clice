#include "test/test.h"
#include "server/config.h"

#include "kota/codec/toml.h"

namespace clice::testing {

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
    auto result = kota::codec::toml::parse<CliceConfig>(R"(
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
    auto result = kota::codec::toml::parse<CliceConfig>("");
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result->rules.empty());
    EXPECT_EQ(result->project.cache_dir, std::string());
}

TEST_CASE(ParseOnlyRules) {
    auto result = kota::codec::toml::parse<CliceConfig>(R"(
[[rules]]
patterns = ["*.h"]
remove = ["-Werror"]
)");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->rules.size(), 1u);
    EXPECT_EQ(result->rules[0].patterns[0], "*.h");
    EXPECT_EQ(result->rules[0].remove[0], "-Werror");
    EXPECT_EQ(result->project.cache_dir, std::string());
}

TEST_CASE(MatchRulesBasic) {
    CliceConfig config;
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
    CliceConfig config;
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
    CliceConfig config;
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
    CliceConfig config;
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
    CliceConfig config;
    config.apply_defaults("");
    EXPECT_TRUE(config.project.cache_dir.empty());
    EXPECT_TRUE(config.project.index_dir.empty());
    EXPECT_TRUE(config.project.logging_dir.empty());
}

TEST_CASE(ApplyDefaultsPreserveSet) {
    CliceConfig config;
    config.project.cache_dir = "/custom";
    config.project.enable_indexing = false;
    config.apply_defaults("/workspace");
    EXPECT_EQ(std::string_view(config.project.cache_dir), "/custom");
    EXPECT_EQ(*config.project.enable_indexing, false);
}

};  // TEST_SUITE(Config)

}  // namespace clice::testing
