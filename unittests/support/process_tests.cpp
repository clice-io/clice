module clice;

import :support.process;
import :unittests.utils;
import :unittests.utils.temp_dir;

namespace clice::testing {
namespace {

ZEST_SUITE(TempRoot) {

#ifndef _WIN32
ZEST_CASE(OwnerOnly) {
    auto root = temp_root();
    ZASSERT(root.has_value());
    auto status = fs::status(*root);
    ZASSERT(status);
    ZEXPECT(status->type == fs::FileType::Directory);
    ZEXPECT(status->permissions == 0700);
}
#endif

ZEST_CASE(ExitedRemoved) {
    TempDir tmp;
    auto dead = std::to_string(std::numeric_limits<int>::max());
    auto self = std::to_string(sys::pid());
    for(auto& name: {dead, self, std::string("socket-1")}) {
        tmp.mkdir(name);
    }
    remove_left_by_exited(tmp.root);
    ZEXPECT(!fs::exists(tmp.path(dead)));
    ZEXPECT(fs::exists(tmp.path(self)));
    ZEXPECT(fs::exists(tmp.path("socket-1")));
}

};  // ZEST_SUITE(TempRoot)

}  // namespace
}  // namespace clice::testing
