module;

#ifndef _WIN32
#include <unistd.h>
#endif

module clice;

import :support.process;
import :unittests.utils;
import :unittests.utils.platform;
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

ZEST_CASE(LinkRefused) {
    TempDir tmp;
    ScopedEnv temp("TMPDIR", tmp.root);
    auto made = temp_root();
    ZASSERT(made.has_value());
    ZASSERT(fs::remove(*made).has_value());
    tmp.mkdir("elsewhere");
    ZASSERT(::symlink(tmp.path("elsewhere").c_str(), made->c_str()) == 0);
    auto refused = temp_root();
    ZASSERT(!refused.has_value());
    ZEXPECT(refused.error().contains(*made));
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
