#include <cstdint>
#include <vector>

#include "test/test.h"
#include "server/service/query.h"
#include "server/state/session_store.h"

namespace clice::testing {
namespace {

TEST_SUITE(IndexQuery) {

TEST_CASE(VisitSkipsUnusableSessions) {
    Workspace workspace;
    SessionStore store;
    IndexQuery query(workspace, store);

    auto make_indexed = [&](std::uint32_t path_id) {
        auto session = store.open(path_id);
        session->file_index.emplace();
        session->symbols.emplace();
        session->ast_dirty = false;
        return session;
    };

    auto clean = make_indexed(1);
    auto dirty = make_indexed(2);
    dirty->ast_dirty = true;
    auto desynced = make_indexed(3);
    desynced->desynced = true;

    // Only the clean, in-sync session may serve cross-file queries; the
    // dirty one degrades to shards and the desynced one's index describes
    // text the user is not looking at.
    std::vector<std::uint32_t> visited;
    query.visit_sessions([&](std::uint32_t path_id, const Session&) -> bool {
        visited.push_back(path_id);
        return true;
    });

    ASSERT_EQ(visited, std::vector<std::uint32_t>{clean->path_id});
}

};  // TEST_SUITE(IndexQuery)

}  // namespace

}  // namespace clice::testing
