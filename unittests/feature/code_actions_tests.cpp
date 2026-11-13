module clice;

import :feature.code_actions;
import :unittests.utils.index_query_tester;
import :unittests.utils.merge_unit;
import :unittests.utils.temp_dir;
import :unittests.utils.tester;
import :vfs.disk_state;
import :unittests.utils;

namespace clice::testing {

namespace {

ZEST_SUITE(resolve_actions, IndexQueryTester) {

/// A definition the index places in the host goes after the host's other
/// definitions without the host's text being read again, which no reply
/// shows.
ZEST_CASE(HostPlacement) {
    TempDir dir;
    auto file = [&](llvm::StringRef name) {
        return CanonicalPath(Spelling::absolute(dir.path(name))).str();
    };
    llvm::StringRef host_text = R"(#include "header.h"
void Shape::move() {}
)";
    Tester host;
    host.kind = CompilationKind::Whole;
    host.add_file(file("header.h"), R"(
struct Shape {
    void draw();
    void move();
};
)");
    host.add_focus(file("host.cpp"), host_text);
    ZASSERT(host.compile());
    Fid host_id;
    merge_unit(project, *host.unit, host_id);
    ZASSERT(!static_cast<bool>(fs::write_file(file("host.cpp"), host_text)));
    project.file_table.disk.look_all();

    add_focus(file("header.h"), R"(
struct Shape {
    void §(draw)draw();
    void move();
};
)");
    ZASSERT(compile());
    auto opened = query();
    auto offset = point("draw");
    auto path = unit->file_path(unit->focus_file().file());
    auto host_path = file("host.cpp");
    bool read = false;
    auto actions =
        feature::resolve_actions(feature::code_actions(*unit, {offset, offset}),
                                 {},
                                 {
                                     .query = &opened,
                                     .file = project.file_table.intern(Spelling::absolute(path)),
                                     .path = path,
                                     .content = unit->focus_file().content(),
                                     .host = host_id,
                                     .host_path = host_path,
                                     .host_style_path = host_path,
                                     .host_text = [&]() -> SourceText* {
                                         read = true;
                                         return nullptr;
                                     },
                                 });
    auto action = ranges::find_if(actions, [](const feature::ResolvedAction& action) {
        return action.title == "Define 'Shape::draw' in host.cpp";
    });
    ZASSERT(action != actions.end());
    ZASSERT(action->host.has_value());
    ZEXPECT(!read);
}

};  // ZEST_SUITE(resolve_actions)

}  // namespace

}  // namespace clice::testing
