#include <chrono>
#include <string>

#include "test/temp_dir.h"
#include "test/test.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/state/session_store.h"
#include "server/worker/worker_pool.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

namespace clice::testing {
namespace {

/// A serialized TUIndex as a worker would ship it: one main TU including one
/// header, with the consumed-content hashes the indexing compilation took
/// from its resident buffers.
std::string make_tu_index(llvm::StringRef header_path,
                          std::uint64_t header_hash,
                          llvm::StringRef main_path,
                          std::uint64_t main_hash) {
    index::TUIndex tu_index;
    tu_index.built_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    tu_index.graph.paths = {header_path.str(), main_path.str()};
    tu_index.graph.path_hashes = {header_hash, main_hash};
    tu_index.graph.locations = {
        {.path_id = 0, .line = 1, .include = std::uint32_t(-1)}
    };

    std::string data;
    llvm::raw_string_ostream os(data);
    tu_index.serialize(os);
    return data;
}

TEST_SUITE(IndexerMerge) {

TEST_CASE(MatchingHashMerges) {
    TempDir dir;
    llvm::StringRef header = "int shared = 1;";
    llvm::StringRef main = "int value = shared;";
    dir.touch("dep.h", header);
    dir.touch("main.cpp", main);

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool(loop);
    ContextResolver contexts(workspace);
    SessionStore sessions;
    Indexer indexer(loop, workspace, pool, contexts, sessions);

    auto data = make_tu_index(dir.path("dep.h"),
                              llvm::xxh3_64bits(header),
                              dir.path("main.cpp"),
                              llvm::xxh3_64bits(main));
    indexer.merge(data.data(), data.size());

    auto main_id = workspace.path_pool.intern(dir.path("main.cpp"));
    auto it = workspace.merged_indices.find(main_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_TRUE(it->second.has_contribution(dir.path("main.cpp")));
    ASSERT_EQ(it->second.content(), main);
    ASSERT_FALSE(it->second.need_update());
}

TEST_CASE(MismatchSkipsMerge) {
    TempDir dir;
    dir.touch("dep.h", "int shared = 1;");
    dir.touch("main.cpp", "int value = shared;");

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool(loop);
    ContextResolver contexts(workspace);
    SessionStore sessions;
    Indexer indexer(loop, workspace, pool, contexts, sessions);

    // The worker consumed an older main file than the disk holds (a write
    // landed during the indexing run): storing disk content next to rows
    // built from other content would corrupt position mapping, so the merge
    // must skip this TU instead.
    auto data = make_tu_index(dir.path("dep.h"),
                              llvm::xxh3_64bits("int shared = 1;"),
                              dir.path("main.cpp"),
                              llvm::xxh3_64bits("int value = 0;"));
    indexer.merge(data.data(), data.size());

    auto main_id = workspace.path_pool.intern(dir.path("main.cpp"));
    auto it = workspace.merged_indices.find(main_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_FALSE(it->second.has_contribution(dir.path("main.cpp")));

    // The skip is only safe because the TU is queued for an explicit redo.
    ASSERT_EQ(indexer.pending_files(), std::size_t(1));
}

TEST_CASE(SkipKeepsOldContribution) {
    TempDir dir;
    llvm::StringRef header = "int shared = 1;";
    llvm::StringRef old_main = "int value = shared;";
    dir.touch("dep.h", header);
    dir.touch("main.cpp", old_main);

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool(loop);
    ContextResolver contexts(workspace);
    SessionStore sessions;
    Indexer indexer(loop, workspace, pool, contexts, sessions);

    auto data = make_tu_index(dir.path("dep.h"),
                              llvm::xxh3_64bits(header),
                              dir.path("main.cpp"),
                              llvm::xxh3_64bits(old_main));
    indexer.merge(data.data(), data.size());

    // The disk moves on; a re-merge of the same stale result must neither
    // store the new content nor sweep away the TU's previous contribution.
    dir.touch("main.cpp", "int value = 2;");
    indexer.merge(data.data(), data.size());

    auto main_id = workspace.path_pool.intern(dir.path("main.cpp"));
    auto& shard = workspace.merged_indices[main_id];
    ASSERT_TRUE(shard.has_contribution(dir.path("main.cpp")));
    ASSERT_EQ(shard.content(), old_main);
}

};  // TEST_SUITE(IndexerMerge)

}  // namespace
}  // namespace clice::testing
