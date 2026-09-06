#include "test/temp_dir.h"
#include "test/test.h"
#include "config/config.h"
#include "sched/configuration.h"
#include "support/filesystem.h"

namespace clice::testing {

namespace {

/// A configuration with two tagged rules, `default_configuration` naming
/// the second, finalized at the temp root (its cache directory is
/// `.clice` there).
Config tagged(const TempDir& tmp) {
    Config config;
    config.default_configuration = "release";
    config.rules.push_back(ConfigRule{.configuration = "debug", .compile_commands = {"debug"}});
    config.rules.push_back(ConfigRule{.configuration = "release", .compile_commands = {"release"}});
    config.finalize(tmp.root.str());
    return config;
}

TEST_SUITE(Configuration) {

TEST_CASE(FallbackNamesDefaultElseFirst) {
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
    EXPECT_TRUE(read_selection("").empty());
};

TEST_CASE(ResolvePrecedence) {
    /// The command line beats the persisted selection, which beats the
    /// fallback.
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_EQ(resolve_configuration(config, ""), "release");

    ASSERT_TRUE(write_selection(config.project.cache_dir, "debug").has_value());
    EXPECT_EQ(resolve_configuration(config, ""), "debug");
    EXPECT_EQ(resolve_configuration(config, "release"), "release");
};

TEST_CASE(UnknownNameFallsBack) {
    /// A name no rule declares is skipped, whichever layer holds it, and a
    /// stale selection stays on disk untouched.
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT_EQ(resolve_configuration(config, "nope"), "release");

    ASSERT_TRUE(write_selection(config.project.cache_dir, "gone").has_value());
    EXPECT_EQ(resolve_configuration(config, ""), "release");
    EXPECT_EQ(read_selection(config.project.cache_dir), "gone");
    EXPECT_EQ(resolve_configuration(config, "debug"), "debug");
};

TEST_CASE(UntaggedIgnoresSelection) {
    /// Without declared tags the configuration is anonymous whatever the
    /// selection file or the command line says.
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
