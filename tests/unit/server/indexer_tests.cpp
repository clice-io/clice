#include <limits>

#include "test/temp_dir.h"
#include "test/test.h"
#include "command/argument_parser.h"
#include "compile/compilation.h"
#include "index/tu_index.h"
#include "server/compiler/context_resolver.h"
#include "server/compiler/indexer.h"
#include "server/state/session_store.h"
#include "server/state/workspace.h"
#include "server/worker/worker_pool.h"

#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

/// Test fixture with friend access to Indexer internals.
struct IndexerFixture {
    using Verdict = Indexer::RequeueVerdict;

    constexpr static unsigned budget = Indexer::max_requeue_attempts;

    kota::event_loop loop;
    Workspace workspace;
    WorkerPool pool{loop};
    ContextResolver contexts{workspace};
    SessionStore sessions;
    Indexer indexer{loop, workspace, pool, contexts, sessions};

    /// Fail the entry's current dispatch: the launch ticket matches.
    Verdict fail(std::uint32_t id, bool crashed) {
        return indexer.note_dispatch_failure(id, ticket(id), crashed);
    }

    /// Fail a dispatch launched with an explicit (possibly stale) ticket.
    Verdict fail_at(std::uint32_t id, std::uint64_t ticket, bool crashed) {
        return indexer.note_dispatch_failure(id, ticket, crashed);
    }

    std::uint64_t ticket(std::uint32_t id) {
        auto it = indexer.reindex_reasons.find(id);
        return it == indexer.reindex_reasons.end() ? std::numeric_limits<std::uint64_t>::max()
                                                   : it->second.ticket;
    }

    unsigned attempts(std::uint32_t id) {
        auto it = indexer.reindex_reasons.find(id);
        return it == indexer.reindex_reasons.end() ? 0u : it->second.requeue_attempts;
    }

    void set_attempts(std::uint32_t id, unsigned n) {
        indexer.reindex_reasons.find(id)->second.requeue_attempts = n;
    }

    /// Consume the queued slot as a dispatch would, so a later enqueue
    /// takes the fresh-slot (mid-flight) path.
    void consume(std::uint32_t id) {
        indexer.pending_ids.erase(id);
    }
};

namespace {

TEST_SUITE(IndexerMerge) {

kota::event_loop loop;
Workspace workspace;
SessionStore store;
WorkerPool pool{loop};
ContextResolver resolver{workspace};
Indexer indexer{loop, workspace, pool, resolver, store};

struct IndexedTU {
    std::string data;     ///< Serialized TUIndex, as a worker would ship it.
    std::string tu_path;  ///< The TU's canonical path inside the index.
};

/// Index a real on-disk file in-process and serialize its TUIndex.
IndexedTU index_file(TempDir& tmp, llvm::StringRef file) {
    std::string resource = std::string(resource_dir());
    std::vector<std::string> args =
        {"clang++", "-fsyntax-only", "-resource-dir", resource, "-c", std::string(file)};

    CompilationParams cp;
    cp.kind = CompilationKind::Indexing;
    cp.directory = std::string(tmp.root);
    for(auto& arg: args) {
        cp.arguments.push_back(arg.c_str());
    }

    auto unit = compile(cp);
    if(!unit.completed()) {
        return {};
    }
    auto tu_index = index::TUIndex::build(unit);
    IndexedTU result;
    result.tu_path = tu_index.graph.paths.back();
    llvm::raw_string_ostream os(result.data);
    tu_index.serialize(os);
    return result;
}

TEST_CASE(MergeSkipsMovedDisk) {
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto src = tmp.path("main.cpp");

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());

    indexer.merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    auto it = workspace.merged_indices.find(path_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");

    // The disk moved on since the rows were indexed: merging them would
    // pair offsets with bytes they were not built from, so the merge must
    // be skipped and the last-known snapshot kept serving.
    tmp.touch("main.cpp", "int renamed() { return 2; }\n");
    indexer.merge(indexed.data.data(), indexed.data.size());
    ASSERT_EQ(it->second.content(), "int value() { return 1; }\n");
    ASSERT_TRUE(it->second.has_contribution(indexed.tu_path));

    // Once the rows describe the settled content again, the merge lands.
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());
    indexer.merge(fresh.data.data(), fresh.data.size());
    ASSERT_EQ(it->second.content(), "int renamed() { return 2; }\n");
}

void open_store(TempDir& tmp, Workspace& workspace) {
    auto store = CacheStore::open(tmp.path("cache"), 1);
    ASSERT_TRUE(store.has_value());
    store->register_namespace(
        {.name = "index", .extension = ".idx", .policy = CachePolicy::Persistent});
    workspace.store.emplace(std::move(*store));
}

TEST_CASE(SaveFlipsShards) {
    TempDir tmp;
    tmp.touch("main.cpp", "int flip_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());

    auto path_id = workspace.path_pool.intern(indexed.tu_path);
    ASSERT_TRUE(workspace.merged_indices.find(path_id)->second.need_rewrite());

    // Named body: a temporary lambda's captures die with the statement
    // while the coroutine frame still references them.
    auto save_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = save_body();
    loop.schedule(task);
    loop.run();

    // Committed and flipped back to the buffer-backed blob; the shard
    // still answers identically.
    auto it = workspace.merged_indices.find(path_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_FALSE(it->second.need_rewrite());
    ASSERT_EQ(it->second.content(), "int flip_value() { return 1; }\n");
    ASSERT_TRUE(it->second.has_contribution(indexed.tu_path));
}

TEST_CASE(MidSaveMergeKept) {
    TempDir tmp;
    tmp.touch("main.cpp", "int first_value() { return 1; }\n");
    auto src = tmp.path("main.cpp");
    open_store(tmp, workspace);

    auto indexed = index_file(tmp, src);
    ASSERT_FALSE(indexed.data.empty());
    indexer.merge(indexed.data.data(), indexed.data.size());
    auto path_id = workspace.path_pool.intern(indexed.tu_path);

    // Prepared before save() starts so the interleaved merge is purely an
    // in-memory event.
    tmp.touch("main.cpp", "int second_value() { return 2; }\n");
    auto fresh = index_file(tmp, src);
    ASSERT_FALSE(fresh.data.empty());

    // The merge task runs when save() suspends at its first commit await:
    // it lands between the serialize snapshot and the flip check, exactly
    // the window the revision guard exists for.
    auto save_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto merge_body = [&]() -> kota::task<> {
        indexer.merge(fresh.data.data(), fresh.data.size());
        co_return;
    };
    auto save_task = save_body();
    auto merge_task = merge_body();
    loop.schedule(save_task);
    loop.schedule(merge_task);
    loop.run();

    // The stale committed blob must not overwrite the newer merge: the
    // shard keeps the new content and stays dirty for the next save.
    auto it = workspace.merged_indices.find(path_id);
    ASSERT_TRUE(it != workspace.merged_indices.end());
    ASSERT_TRUE(it->second.need_rewrite());
    ASSERT_EQ(it->second.content(), "int second_value() { return 2; }\n");

    auto again_body = [&]() -> kota::task<> {
        co_await indexer.save();
    };
    auto task = again_body();
    loop.schedule(task);
    loop.run();

    it = workspace.merged_indices.find(path_id);
    ASSERT_FALSE(it->second.need_rewrite());
    ASSERT_EQ(it->second.content(), "int second_value() { return 2; }\n");
}

};  // TEST_SUITE(IndexerMerge)

TEST_SUITE(IndexerRequeue) {

TEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/a.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; ++i) {
        ASSERT_EQ(int(f.fail(id, /*crashed=*/false)), int(IndexerFixture::Verdict::Requeued));
    }
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(CrashSpendsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/poison.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

    for(unsigned i = 0; i < IndexerFixture::budget; ++i) {
        ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    }
    ASSERT_EQ(f.attempts(id), IndexerFixture::budget);

    // A preemption still requeues a file whose crash budget is spent:
    // dropping it would erase the pending state and serve the stale
    // shard as fresh. Only the next crash gives up.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/false)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(f.attempts(id), IndexerFixture::budget);

    // Giving up clears the pending slot: nothing is left to requeue, and
    // the stale shard serves as fresh — the accepted cost of abandoning.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::GaveUp));
    ASSERT_FALSE(f.indexer.pending_reason(id).has_value());
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/edited.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.ticket(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending slot.
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(int(f.fail_at(id, stale, /*crashed=*/true)),
              int(IndexerFixture::Verdict::Superseded));
    ASSERT_EQ(f.attempts(id), 0u);
    ASSERT_TRUE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(DepsDowngradeKeepsDebt) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/c.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    // The content pass is dispatched; a deps-only cascade lands mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // debt or the stale shard stops being suppressed.
    f.consume(id);
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(*f.indexer.pending_reason(id)), int(ReindexReason::DepsOnly));

    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(*f.indexer.pending_reason(id)), int(ReindexReason::ContentChanged));
    ASSERT_EQ(f.attempts(id), 1u);
}

TEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/d.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);
    f.set_attempts(id, IndexerFixture::budget);

    // A deps-only enqueue lands mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(int(f.fail_at(id, launch, /*crashed=*/true)), int(IndexerFixture::Verdict::GaveUp));
    ASSERT_FALSE(f.indexer.pending_reason(id).has_value());
}

TEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/gone.cpp");
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Dropped));
}

TEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = f.workspace.path_pool.intern("/proj/fixed.cpp");
    f.indexer.enqueue(id, ReindexReason::ContentChanged);

    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    ASSERT_EQ(f.attempts(id), 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.indexer.enqueue(id, ReindexReason::ContentChanged);
    ASSERT_EQ(f.attempts(id), 0u);

    // A deps-only cascade is not new content and keeps the ledger.
    ASSERT_EQ(int(f.fail(id, /*crashed=*/true)), int(IndexerFixture::Verdict::Requeued));
    f.indexer.enqueue(id, ReindexReason::DepsOnly);
    ASSERT_EQ(f.attempts(id), 1u);
}

};  // TEST_SUITE(IndexerRequeue)

}  // namespace
}  // namespace clice::testing
