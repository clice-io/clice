module;

#include "modules/prelude.h"

module clice;

import :config.config;
import :project.configuration;
import :tests.unit.test.temp_dir;
import :tests.unit.test.test;
import :vfs.path;

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
    ZEXPECT(fallback_configuration(config) == "release");

    config.default_configuration = "nope";
    ZEXPECT(fallback_configuration(config) == "debug");

    Config untagged;
    untagged.rules.push_back(ConfigRule{.compile_commands = {"."}});
    untagged.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(fallback_configuration(untagged).empty());
};

ZEST_CASE(SelectionRoundTrips) {
    TempDir tmp;
    auto cache_dir = tmp.path(".clice");
    ZEXPECT(read_selection(cache_dir).empty());
    ZASSERT(write_selection(cache_dir, "release"));
    ZEXPECT(read_selection(cache_dir) == "release");
    ZASSERT(write_selection(cache_dir, "debug"));
    ZEXPECT(read_selection(cache_dir) == "debug");
    ZEXPECT(vfs::exists(path::join(cache_dir, "state.json")));

    tmp.touch(".clice/state.json", "not json");
    ZEXPECT(read_selection(cache_dir).empty());
    ZEXPECT(read_file(path::join(cache_dir, "state.json")).value_or("") == "not json");
    ZEXPECT(read_selection("").empty());
};

ZEST_CASE(SelectionWriteFails) {
    TempDir tmp;
    ZEXPECT(!write_selection("", "release").has_value());
    tmp.touch("blocked", "x");
    ZEXPECT(!write_selection(tmp.path("blocked"), "release").has_value());
};

ZEST_CASE(CheckRequested) {
    TempDir tmp;
    auto config = tagged(tmp);
    ZEXPECT(declares_configuration(config, "debug"));
    ZEXPECT(!declares_configuration(config, "nope"));
    ZEXPECT(check_requested_configuration(config, ""));
    ZEXPECT(check_requested_configuration(config, "release"));
    ZEXPECT(!check_requested_configuration(config, "nope"));
};

ZEST_CASE(ResolvePrecedence) {
    TempDir tmp;
    auto config = tagged(tmp);
    ZEXPECT(resolve_configuration(config, "") == "release");

    ZASSERT(write_selection(config.project.cache_dir, "debug"));
    ZEXPECT(resolve_configuration(config, "") == "debug");
    ZEXPECT(resolve_configuration(config, "release") == "release");
};

ZEST_CASE(UnknownNameFallsBack) {
    TempDir tmp;
    auto config = tagged(tmp);
    ZEXPECT(resolve_configuration(config, "nope") == "release");

    ZASSERT(write_selection(config.project.cache_dir, "gone"));
    ZEXPECT(resolve_configuration(config, "") == "release");
    ZEXPECT(read_selection(config.project.cache_dir) == "gone");
    ZEXPECT(resolve_configuration(config, "debug") == "debug");
};

ZEST_CASE(UntaggedIgnoresSelection) {
    TempDir tmp;
    Config config;
    config.rules.push_back(ConfigRule{.compile_commands = {"."}});
    config.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZASSERT(write_selection(config.project.cache_dir, "release"));
    ZEXPECT(resolve_configuration(config, "").empty());
    ZEXPECT(resolve_configuration(config, "release").empty());
};

};  // ZEST_SUITE(Configuration)

}  // namespace

}  // namespace clice::testing
