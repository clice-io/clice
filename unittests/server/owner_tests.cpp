module clice;

import :server.master;
import :server.served_project;
import :settings.settings;
import :unittests.server.served_project_fixture;
import :unittests.utils;
import :unittests.utils.cdb_helper;
import :unittests.utils.temp_dir;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(Owners) {

ZEST_CASE(ListingBeatsRecordedMember) {
    /// A file the nested project recorded as a provisional member, which
    /// the enclosing project's database lists: the listing serves it, the
    /// deeper root notwithstanding. A record is no listing.
    TempDir tmp;
    tmp.touch("sub/x.cpp", "int x;\n");
    ServedProjectFixture fixture(tmp.path("sub"));
    auto& nested = *fixture.served;
    auto outer = std::make_shared<ServedProject>(fixture.server,
                                                 CanonicalPath(Spelling::absolute(tmp.root)));
    fixture.server.projects.push_back(outer);
    ServedProjectFixture::hold_flushes(*outer);
    auto file = fixture.intern(tmp.path("sub/x.cpp"));
    ZASSERT(nested.project.record_provisional(file));
    nested.project.update_members({});
    ZASSERT(nested.project.is_unit(file));
    ZASSERT(&fixture.server.owner_of(file) == &nested);

    write_cdb(tmp,
              outer->project.cdb,
              build_cdb_json({
                  {tmp.root, tmp.path("sub/x.cpp"), {}}
    }));
    ZEXPECT(&fixture.server.owner_of(file) == outer.get());
}

ZEST_CASE(MoveWatchesSettings) {
    /// A context choice moves the document to a project whose root holds
    /// it deeper down: the directories on the way to it decide what that
    /// project loads, so a configuration file written there asks for a
    /// restart.
    TempDir tmp;
    tmp.touch("b/deep/sub/x.cpp", "int x;\n");
    ServedProjectFixture fixture(tmp.path("a"));
    auto& server = fixture.server;
    auto other =
        std::make_shared<ServedProject>(server, CanonicalPath(Spelling::absolute(tmp.path("b"))));
    // Nothing to compile as it moves.
    other->project.settings.project.readonly = ReadonlyMode::On;
    server.projects.push_back(other);
    ServedProjectFixture::hold_flushes(*other);
    auto session = fixture.open(tmp.path("b/deep/sub/x.cpp"), "int x;\n");
    server.watch_settings();

    server.lifecycle = ServerLifecycle::Ready;
    server.choices[session->path_id] = {.project = other.get()};
    server.update_owners();
    ZASSERT(&server.owner_of(session->path_id) == other.get());

    tmp.touch("b/deep/sub/clice.toml", "");
    server.look_at_settings();
    ZEXPECT(server.restart_reasons.size() == 1u);
}

};  // ZEST_SUITE(Owners)

}  // namespace
}  // namespace clice::testing
