module clice;

import :settings.settings;
import :settings.configuration;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.path;

namespace clice::testing {

namespace {

Settings tagged(const TempDir& tmp) {
    Settings settings;
    settings.default_configuration = "release";
    settings.rules.push_back(Rule{.configuration = "debug", .compile_commands = {"debug"}});
    settings.rules.push_back(Rule{.configuration = "release", .compile_commands = {"release"}});
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    return settings;
}

ZEST_SUITE(Configuration) {

ZEST_CASE(FallbackDefaultElseFirst) {
    TempDir tmp;
    auto settings = tagged(tmp);
    ZEXPECT(settings.fallback_configuration() == "release");

    settings.default_configuration.clear();
    ZEXPECT(settings.fallback_configuration() == "debug");

    Settings untagged;
    untagged.rules.push_back(Rule{.compile_commands = {"."}});
    untagged.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZEXPECT(untagged.fallback_configuration().empty());
};

ZEST_CASE(SelectionRoundTrips) {
    TempDir tmp;
    auto cache_dir = tmp.path(".clice");
    ZEXPECT(read_selected_configuration(cache_dir).empty());
    ZASSERT(write_selected_configuration(cache_dir, "release"));
    ZEXPECT(read_selected_configuration(cache_dir) == "release");
    ZASSERT(write_selected_configuration(cache_dir, "debug"));
    ZEXPECT(read_selected_configuration(cache_dir) == "debug");
    ZEXPECT(fs::exists(path::join(cache_dir, "state.json")));

    tmp.touch(".clice/state.json", "not json");
    ZEXPECT(read_selected_configuration(cache_dir).empty());
    ZEXPECT(read_file(path::join(cache_dir, "state.json")).value_or("") == "not json");
    ZEXPECT(read_selected_configuration("").empty());
};

ZEST_CASE(SelectionWriteFails) {
    TempDir tmp;
    ZEXPECT(!write_selected_configuration("", "release").has_value());
    tmp.touch("blocked", "x");
    ZEXPECT(!write_selected_configuration(tmp.path("blocked"), "release").has_value());
};

ZEST_CASE(Declares) {
    TempDir tmp;
    auto settings = tagged(tmp);
    ZEXPECT(settings.declares("debug"));
    ZEXPECT(!settings.declares("nope"));
};

ZEST_CASE(ChoosePrecedence) {
    TempDir tmp;
    auto settings = tagged(tmp);
    std::vector<SettingsIssue> issues;
    ZEXPECT(choose_configuration(settings, "", true, issues) == "release");

    ZASSERT(write_selected_configuration(settings.project.cache_dir, "debug"));
    ZEXPECT(choose_configuration(settings, "", true, issues) == "debug");
    ZEXPECT(choose_configuration(settings, "release", true, issues) == "release");
    ZEXPECT(issues.empty());
};

ZEST_CASE(UnknownRequest) {
    /// Run scripted, a name no rule declares is an error; served, it is
    /// skipped with a warning.
    TempDir tmp;
    auto settings = tagged(tmp);
    std::vector<SettingsIssue> strict;
    ZEXPECT(choose_configuration(settings, "nope", true, strict) == "release");
    ZASSERT(strict.size() == 1U);
    ZEXPECT(strict[0].severity == SettingsIssue::Severity::Error);
    ZEXPECT(strict[0].message ==
            "--configuration nope names no rule's configuration (debug, release)");

    std::vector<SettingsIssue> served;
    ZEXPECT(choose_configuration(settings, "nope", false, served) == "release");
    ZASSERT(served.size() == 1U);
    ZEXPECT(served[0].severity == SettingsIssue::Severity::Warning);
};

ZEST_CASE(StaleSelection) {
    TempDir tmp;
    auto settings = tagged(tmp);
    ZASSERT(write_selected_configuration(settings.project.cache_dir, "gone"));
    std::vector<SettingsIssue> issues;
    ZEXPECT(choose_configuration(settings, "", true, issues) == "release");
    ZASSERT(issues.size() == 1U);
    ZEXPECT(issues[0].severity == SettingsIssue::Severity::Warning);
    ZEXPECT(read_selected_configuration(settings.project.cache_dir) == "gone");
    ZEXPECT(choose_configuration(settings, "debug", true, issues) == "debug");
};

ZEST_CASE(UntaggedIgnoresSelection) {
    TempDir tmp;
    Settings settings;
    settings.rules.push_back(Rule{.compile_commands = {"."}});
    settings.finalize(CanonicalPath(Spelling::absolute(tmp.root)));
    ZASSERT(write_selected_configuration(settings.project.cache_dir, "release"));
    std::vector<SettingsIssue> issues;
    ZEXPECT(choose_configuration(settings, "", true, issues).empty());
    ZEXPECT(issues.empty());
};

};  // ZEST_SUITE(Configuration)

}  // namespace

}  // namespace clice::testing
