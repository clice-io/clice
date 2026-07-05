#include "test/test.h"
#include "server/workspace/invalidator.h"

namespace clice::testing {
namespace {

TEST_SUITE(Invalidator) {

TEST_CASE(EmptyBatchNoEffects) {
    Workspace workspace;
    SessionStore store;
    Invalidator invalidator(workspace, store);

    auto dirty = invalidator.apply({});

    ASSERT_TRUE(dirty.empty());
}

};  // TEST_SUITE(Invalidator)

}  // namespace
}  // namespace clice::testing
