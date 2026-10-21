module;

#include "modules/prelude.h"

module clice;

import :index.project_index;
import :sched.index.pending_table;
import :sched.index.pump;
import :unittests.utils.forwarding_database;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(IndexPumpRequeue) {

ZEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/a.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; i += 1) {
        ZASSERT(int(f.fail(id, PendingTable::Failure::Preempted)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.attempts(id) == 0u);
    ZASSERT(f.pump.pending_reason(id));
}

ZEST_CASE(OwnCrashGivesUp) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/poison.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    // The run named by its dying worker would crash the next one too: the
    // file waits for its content to change.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Crashed)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
}

ZEST_CASE(LostSpendsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/poison.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    for(unsigned i = 0; i < IndexerFixture::budget; i += 1) {
        ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.attempts(id) == IndexerFixture::budget);

    // A preemption still requeues a file whose budget is spent: dropping
    // it would erase the pending state and serve the stale shard as fresh.
    // Only the next lost run gives up.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Preempted)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.attempts(id) == IndexerFixture::budget);

    // Giving up clears the pending slot: nothing is left to requeue, and
    // the stale shard serves as fresh — the accepted cost of abandoning.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/edited.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.ticket(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending slot.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(int(f.fail_at(id, stale, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Superseded));
    ZASSERT(f.attempts(id) == 0u);
    ZASSERT(f.pump.pending_reason(id));
}

ZEST_CASE(DepsDowngradeKeepsDebt) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/c.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    // The content pass is dispatched; a deps-only cascade arrives mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // debt or the stale shard stops being suppressed.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(*f.pump.pending_reason(id)) == int(ReindexReason::DepsOnly));

    ZASSERT(int(f.fail_at(id, launch, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(*f.pump.pending_reason(id)) == int(ReindexReason::ContentChanged));
    ZASSERT(f.attempts(id) == 1u);
}

ZEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/d.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);
    f.set_attempts(id, IndexerFixture::budget);

    // A deps-only enqueue arrives mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(f.fail_at(id, launch, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.pump.pending_reason(id).has_value());
}

ZEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/gone.cpp"));
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(AttemptWaitPerTicket) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/waited.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.ticket(id);

    bool first_woke = false;
    auto first_body = [&]() -> kota::task<> {
        co_await f.pump.await_attempt(id);
        first_woke = true;
    };
    auto first = first_body();
    f.loop.schedule(first);
    f.loop.run();

    // A requeue arrives mid-flight: the entry stays pending under a fresh
    // ticket, and a new waiter binds to that newer attempt.
    f.consume(id);
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    bool second_woke = false;
    auto second_body = [&]() -> kota::task<> {
        co_await f.pump.await_attempt(id);
        second_woke = true;
    };
    auto second = second_body();
    f.loop.schedule(second);
    f.loop.run();

    // The flight's completion covers the ticket its waiter observed, no
    // matter the requeue: await_attempt promises one attempt, and holding
    // the waiter through every follow-up would park a feature request for
    // as long as edits keep arriving.
    f.wake_waiters(id, launch);
    ZASSERT(first_woke);
    ZASSERT(!second_woke);

    f.wake_waiters(id, f.ticket(id));
    ZASSERT(second_woke);
}

ZEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/fixed.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);

    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.attempts(id) == 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(f.attempts(id) == 0u);

    // A deps-only cascade is not new content and keeps the attempt count.
    ZASSERT(int(f.fail(id, PendingTable::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    f.pump.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(f.attempts(id) == 1u);
}

ZEST_CASE(RoundSnapshotBoundary) {
    IndexerFixture f;
    // Manual rounds: the tail schedule() must no-op so the boundary between
    // the two rounds stays observable.
    f.project.config.project.enable_indexing.value = false;

    auto a = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    auto b = f.project.file_table.intern(Spelling::absolute("/fake/b.cpp"));
    auto c = f.project.file_table.intern(Spelling::absolute("/fake/c.cpp"));

    f.pump.enqueue(a, ReindexReason::ContentChanged);
    f.pump.enqueue(b, ReindexReason::ContentChanged);

    // Grow the queue from inside the round: the first Report enqueues a
    // third file, which must arrive past the round snapshot and wait for the
    // next round instead of being consumed by this one.
    bool grew = false;
    IndexPump::Progress first_end;
    auto conn = f.pump.on_progress_changed.connect([&] {
        auto& progress = f.pump.progress();
        if(progress.stage == IndexPump::Progress::Stage::Report && !grew) {
            grew = true;
            f.pump.enqueue(c, ReindexReason::ContentChanged);
        }
        if(progress.stage == IndexPump::Progress::Stage::End && first_end.total == 0) {
            first_end = progress;
        }
    });

    f.run_round();

    ZASSERT(grew);
    ZASSERT(first_end.total == 2u);
    ZASSERT(first_end.dispatched == 2u);
    ZASSERT(first_end.completed == 2u);
    ZASSERT(f.pump.pending_files() == 1u);

    f.run_round();

    ZASSERT(f.pump.pending_files() == 0u);
    ZASSERT(f.pump.failed().size() == 3u);
    ZASSERT(f.pump.is_idle());
}

ZEST_CASE(PauseResumesRound) {
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = false;

    auto a = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    auto b = f.project.file_table.intern(Spelling::absolute("/fake/b.cpp"));
    f.pump.enqueue(a, ReindexReason::ContentChanged);
    f.pump.enqueue(b, ReindexReason::ContentChanged);

    // Pause from inside the round (the first Report), resume from a
    // separately scheduled task: the feeder must park on the resume event
    // and drain the rest of the round afterwards.
    bool paused = false;
    auto conn = f.pump.on_progress_changed.connect([&] {
        if(f.pump.progress().stage == IndexPump::Progress::Stage::Report && !paused) {
            paused = true;
            f.pump.pause_indexing();
        }
    });

    auto resume_body = [&]() -> kota::task<> {
        co_await kota::yield();
        f.pump.resume_indexing();
    };
    auto round = f.round_task();
    auto resumer = resume_body();
    f.loop.schedule(round);
    f.loop.schedule(resumer);
    f.loop.run();

    ZASSERT(paused);
    ZASSERT(f.pump.pending_files() == 0u);
    ZASSERT(f.pump.failed().size() == 2u);
    ZASSERT(f.pump.is_idle());
}

};  // ZEST_SUITE(IndexPumpRequeue)

/// The index's serving-neutral change reports and the pump taking them:
/// every index-changing source reports debt and index changes, the save
/// carries the pump's debt snapshot both ways.
ZEST_SUITE(IndexPumpReports) {

ZEST_CASE(MergeReportsIndexChanged) {
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    ZASSERT(f.merge(indexed.data));

    ZASSERT(llvm::is_contained(notified,
                               f.project.file_table.intern(Spelling::absolute(indexed.tu_path))));
}

ZEST_CASE(DropReportsServedIndex) {
    // drop_index deletes entries an index-served session may already have
    // consumed; the report must carry every affected file so the serving
    // side can refresh — dropped entries change answers exactly like merged
    // entries do, and no later merge or compile is owed to cover them.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));

    auto tu_id = f.project.file_table.intern(Spelling::absolute(indexed.tu_path));
    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    index::IndexReport report;
    f.project.index.drop(tu_id, report);
    ZASSERT(llvm::is_contained(report.index_changed(), tu_id));
    ZASSERT(llvm::is_contained(report.index_changed(), header_id));

    f.pump.apply_report(report);
    ZASSERT(llvm::is_contained(notified, header_id));
}

ZEST_CASE(RetireReportsIndexChanged) {
    // The save-side recovery source: a shard retired by the compaction
    // vanishes from memory, which changes index-served answers exactly
    // like a merge — the report must say so.
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("dep.h", "#pragma once\ninline int dep() { return 1; }\n");
    tmp.touch("main.cpp", "#include \"dep.h\"\nint use() { return dep(); }\n");
    open_store(tmp, f.project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();

    // The TU stops including the header: its contribution dies, and the
    // next save retires the header's shard entirely.
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    auto second = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!second.data.empty());
    ZASSERT(f.merge(second.data));

    auto header_id = f.project.file_table.intern(Spelling::absolute(tmp.path("dep.h")));
    ZASSERT(f.project.index.shard(header_id) != nullptr);

    llvm::SmallVector<Fid> notified;
    auto conn = f.pump.on_index_changed.connect(
        [&](llvm::ArrayRef<Fid> ids) { notified.append(ids.begin(), ids.end()); });
    f.save();

    ZASSERT(!f.project.index.shard(header_id));
    ZASSERT(llvm::is_contained(notified, header_id));
}

ZEST_CASE(BoostRearmsIdleTimer) {
    // s#9: a boost colliding with an already-armed idle timer must re-arm
    // it to fire now. Un-fixed, this test waits out the full idle window
    // below instead of finishing promptly.
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = true;
    f.project.config.project.idle_timeout_ms.value = 60'000;

    auto id = f.project.file_table.intern(Spelling::absolute("/fake/a.cpp"));
    f.pump.enqueue(id, ReindexReason::ContentChanged);
    f.pump.schedule();
    f.pump.boost(id);
    f.loop.run();

    ZASSERT(f.pump.is_idle());
    ZASSERT(f.pump.failed().size() == 1u);
}

ZEST_CASE(BoostSparesRunningRound) {
    // A file boosted while the running round has it in flight is queued
    // past the round and leads the next one. Moved into the running round,
    // it would run there a second time and push the round's last file out.
    IndexerFixture f;
    f.project.config.project.enable_indexing.value = true;
    f.project.config.project.idle_timeout_ms.value = 0;

    llvm::SmallVector<Fid> files;
    for(auto name: {"/fake/a.cpp", "/fake/b.cpp", "/fake/c.cpp"}) {
        files.push_back(f.project.file_table.intern(Spelling::absolute(name)));
        f.pump.enqueue(files.back(), ReindexReason::DepsOnly);
    }

    // Asked for each dispatched deps-only slot: the boost arrives while the
    // second file is taken.
    std::vector<llvm::SmallVector<Fid>> rounds(1);
    f.pump.compiled_by_session = [&](Fid id) {
        if(rounds.size() == 1 && rounds.front().size() == 1) {
            f.pump.boost(id);
        }
        rounds.back().push_back(id);
        return false;
    };
    auto conn = f.pump.on_progress_changed.connect([&] {
        if(f.pump.progress().stage == IndexPump::Progress::Stage::End) {
            rounds.emplace_back();
        }
    });
    f.pump.schedule();
    f.loop.run();

    ZEXPECT(rounds.size() == 3u);
    ZEXPECT(rounds[0] == files);
    ZEXPECT(rounds[1] == llvm::SmallVector<Fid>{files[1]});
    ZEXPECT(f.pump.is_idle());
}

ZEST_CASE(EmptyRoundBuildsSearch) {
    // A session finding nothing to index still owes the search index the
    // idle rebuild the previous one never reached.
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    IndexerFixture f;
    open_store(tmp, f.project);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();
    ZASSERT(!f.project.index.search_index().loaded());

    f.run_round();
    ZEXPECT(f.project.index.search_index().loaded());
}

ZEST_CASE(EmptyRoundDiscardsDamagedDatabase) {
    // A scan met a damaged page after the load: a session finding nothing
    // to index still discards the database at its idle save.
    struct DamagedPage final : ForwardingDatabase {
        using ForwardingDatabase::ForwardingDatabase;

        bool damaged = false;

        bool* condemned;

        bool corrupted() const override {
            return damaged || real->corrupted();
        }

        void condemn() override {
            *condemned = true;
            real->condemn();
        }
    };

    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    IndexerFixture f;
    open_store(tmp, f.project);
    bool condemned = false;
    auto spy = std::make_unique<DamagedPage>(std::move(f.project.database));
    spy->condemned = &condemned;
    auto& page = *spy;
    f.project.database = std::move(spy);
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());
    ZASSERT(f.merge(indexed.data));
    f.save();
    ZASSERT(!f.project.index.damaged());

    page.damaged = true;
    ZEXPECT(f.project.index.damaged());
    f.run_round();
    // The recovery replaced the database, the spy with it.
    ZEXPECT(condemned);
}

};  // ZEST_SUITE(IndexPumpReports)

}  // namespace
}  // namespace clice::testing
