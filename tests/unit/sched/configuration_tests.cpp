#include "test/temp_dir.h"
#include "test/test.h"
#include "config/config.h"
#include "sched/configuration.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

Config tagged(const TempDir& tmp) {
    Config config;
    config.default_configuration = "release";
    config.rules.push_back(ConfigRule{.configuration = "debug", .compile_commands = {"debug"}});
    config.rules.push_back(ConfigRule{.configuration = "release", .compile_commands = {"release"}});
    config.finalize(tmp.root.str());
    return config;
}

TEST_SUITE(Configuration) {

TEST_CASE(FallbackDefaultElseFirst) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_EQ(fallback_configuration(config), "release");

    config.default_configuration = "nope";
    EXPECT_EQ(fallback_configuration(config), "debug");

    Config untagged;
    untagged.rules.push_back(ConfigRule{.compile_commands = {"."}});
    untagged.finalize(tmp.root.str());
    EXPECT_TRUE(fallback_configuration(untagged).empty());
};

TEST_CASE(SelectionRoundTrips) {
    TempDir tmp;
    auto cache_dir = tmp.path(".clice");
    EXPECT_TRUE(read_selection(cache_dir).empty());
    ASSERT_TRUE(write_selection(cache_dir, "release").has_value());
    EXPECT_EQ(read_selection(cache_dir), "release");
    ASSERT_TRUE(write_selection(cache_dir, "debug").has_value());
    EXPECT_EQ(read_selection(cache_dir), "debug");
    EXPECT_TRUE(fs::exists(path::join(cache_dir, "state.json")));

    tmp.touch(".clice/state.json", "not json");
    EXPECT_TRUE(read_selection(cache_dir).empty());
    EXPECT_EQ(fs::read(path::join(cache_dir, "state.json")).value_or(""), "not json");
    EXPECT_TRUE(read_selection("").empty());
};

TEST_CASE(SelectionWriteFails) {
    TempDir tmp;
    EXPECT_FALSE(write_selection("", "release").has_value());
    tmp.touch("blocked", "x");
    EXPECT_FALSE(write_selection(tmp.path("blocked"), "release").has_value());
};

TEST_CASE(CheckRequested) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_TRUE(declares_configuration(config, "debug"));
    EXPECT_FALSE(declares_configuration(config, "nope"));
    EXPECT_TRUE(check_requested_configuration(config, ""));
    EXPECT_TRUE(check_requested_configuration(config, "release"));
    EXPECT_FALSE(check_requested_configuration(config, "nope"));
};

TEST_CASE(ResolvePrecedence) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_EQ(resolve_configuration(config, ""), "release");

    ASSERT_TRUE(write_selection(config.project.cache_dir, "debug").has_value());
    EXPECT_EQ(resolve_configuration(config, ""), "debug");
    EXPECT_EQ(resolve_configuration(config, "release"), "release");
};

TEST_CASE(UnknownNameFallsBack) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_EQ(resolve_configuration(config, "nope"), "release");

    ASSERT_TRUE(write_selection(config.project.cache_dir, "gone").has_value());
    EXPECT_EQ(resolve_configuration(config, ""), "release");
    EXPECT_EQ(read_selection(config.project.cache_dir), "gone");
    EXPECT_EQ(resolve_configuration(config, "debug"), "debug");
};

TEST_CASE(UntaggedIgnoresSelection) {
    TempDir tmp;
    Config config;
    config.rules.push_back(ConfigRule{.compile_commands = {"."}});
    config.finalize(tmp.root.str());
    ASSERT_TRUE(write_selection(config.project.cache_dir, "release").has_value());
    EXPECT_TRUE(resolve_configuration(config, "").empty());
    EXPECT_TRUE(resolve_configuration(config, "release").empty());
};

};  // TEST_SUITE(Configuration)

}  // namespace

}  // namespace clice::testing
