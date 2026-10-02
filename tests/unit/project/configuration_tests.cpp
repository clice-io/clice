#include "test/temp_dir.h"
#include "test/test.h"
#include "config/config.h"
#include "project/configuration.h"
#include "vfs/path.h"

namespace clice::testing {

namespace {

Config tagged(const TempDir& tmp) {
    Config config;
    config.default_configuration = "release";
    config.rules.push_back(ConfigRule{.configuration = "debug", .compile_commands = {"debug"}});
    config.rules.push_back(ConfigRule{.configuration = "release", .compile_commands = {"release"}});
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    return config;
}

ZEST_SUITE(Configuration) {

ZEST_CASE(FallbackDefaultElseFirst) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT(fallback_configuration(config) == "release");

    config.default_configuration = "nope";
    EXPECT(fallback_configuration(config) == "debug");

    Config untagged;
    untagged.rules.push_back(ConfigRule{.compile_commands = {"."}});
    untagged.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    EXPECT(fallback_configuration(untagged).empty());
};

ZEST_CASE(SelectionRoundTrips) {
    TempDir tmp;
    auto cache_dir = tmp.path(".clice");
    EXPECT(read_selection(cache_dir).empty());
    ASSERT(write_selection(cache_dir, "release"));
    EXPECT(read_selection(cache_dir) == "release");
    ASSERT(write_selection(cache_dir, "debug"));
    EXPECT(read_selection(cache_dir) == "debug");
    EXPECT(vfs::exists(path::join(cache_dir, "state.json")));

    tmp.touch(".clice/state.json", "not json");
    EXPECT(read_selection(cache_dir).empty());
    EXPECT(read_file(path::join(cache_dir, "state.json")).value_or("") == "not json");
    EXPECT(read_selection("").empty());
};

ZEST_CASE(SelectionWriteFails) {
    TempDir tmp;
    EXPECT(!write_selection("", "release").has_value());
    tmp.touch("blocked", "x");
    EXPECT(!write_selection(tmp.path("blocked"), "release").has_value());
};

ZEST_CASE(CheckRequested) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT(declares_configuration(config, "debug"));
    EXPECT(!declares_configuration(config, "nope"));
    EXPECT(check_requested_configuration(config, ""));
    EXPECT(check_requested_configuration(config, "release"));
    EXPECT(!check_requested_configuration(config, "nope"));
};

ZEST_CASE(ResolvePrecedence) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT(resolve_configuration(config, "") == "release");

    ASSERT(write_selection(config.project.cache_dir, "debug"));
    EXPECT(resolve_configuration(config, "") == "debug");
    EXPECT(resolve_configuration(config, "release") == "release");
};

ZEST_CASE(UnknownNameFallsBack) {
    TempDir tmp;
    auto config = tagged(tmp);
    EXPECT(resolve_configuration(config, "nope") == "release");

    ASSERT(write_selection(config.project.cache_dir, "gone"));
    EXPECT(resolve_configuration(config, "") == "release");
    EXPECT(read_selection(config.project.cache_dir) == "gone");
    EXPECT(resolve_configuration(config, "debug") == "debug");
};

ZEST_CASE(UntaggedIgnoresSelection) {
    TempDir tmp;
    Config config;
    config.rules.push_back(ConfigRule{.compile_commands = {"."}});
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ASSERT(write_selection(config.project.cache_dir, "release"));
    EXPECT(resolve_configuration(config, "").empty());
    EXPECT(resolve_configuration(config, "release").empty());
};

};  // ZEST_SUITE(Configuration)

}  // namespace

}  // namespace clice::testing
