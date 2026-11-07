module clice;

import :server.master;
import :server.served_project;
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

};  // ZEST_SUITE(Owners)

}  // namespace
}  // namespace clice::testing
