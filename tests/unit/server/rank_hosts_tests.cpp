#include "test/test.h"
#include "server/state/workspace.h"

namespace clice::testing {
namespace {

TEST_SUITE(RankHosts) {

TEST_CASE(StemMatchWins) {
    Workspace ws;
    auto header = ws.path_pool.intern("/proj/src/utils.h");
    auto stem = ws.path_pool.intern("/other/utils.cpp");
    auto same_dir = ws.path_pool.intern("/proj/src/main.cpp");

    auto ranked = ws.rank_hosts(header, {same_dir, stem});
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], stem);
    EXPECT_EQ(ranked[1], same_dir);
}

TEST_CASE(SameDirBeatsProximity) {
    Workspace ws;
    auto header = ws.path_pool.intern("/proj/src/utils.h");
    auto same_dir = ws.path_pool.intern("/proj/src/zzz.cpp");
    auto sibling_dir = ws.path_pool.intern("/proj/lib/aaa.cpp");

    auto ranked = ws.rank_hosts(header, {sibling_dir, same_dir});
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], same_dir);
}

TEST_CASE(ProximityThenLexicographic) {
    Workspace ws;
    auto header = ws.path_pool.intern("/proj/a/b/utils.h");
    auto near = ws.path_pool.intern("/proj/a/main.cpp");
    auto far = ws.path_pool.intern("/proj/x/main.cpp");

    auto ranked = ws.rank_hosts(header, {far, near});
    ASSERT_EQ(ranked.size(), 2u);
    EXPECT_EQ(ranked[0], near);

    auto first = ws.path_pool.intern("/proj/x/aaa.cpp");
    auto second = ws.path_pool.intern("/proj/x/bbb.cpp");
    auto tie = ws.rank_hosts(header, {second, first});
    ASSERT_EQ(tie.size(), 2u);
    EXPECT_EQ(tie[0], first);
}

};  // TEST_SUITE(RankHosts)

}  // namespace
}  // namespace clice::testing
