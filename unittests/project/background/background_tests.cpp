module clice;

import :index.project_index;
import :index.project_index.manifest;
import :project.background;
import :unittests.utils.cdb_helper;
import :unittests.utils.forwarding_database;
import :unittests.utils.index_internals;
import :unittests.utils.indexer_fixture;
import :unittests.utils.temp_dir;
import :unittests.utils;
import :vfs.ids;
import :vfs.path;

namespace clice::testing {
namespace {

ZEST_SUITE(BackgroundIndexRequeue) {

ZEST_CASE(PreemptionKeepsBudget) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/a.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);

    // A preemption under memory pressure requeues without spending the
    // crash budget, no matter how often it repeats.
    for(unsigned i = 0; i < 2 * IndexerFixture::budget; i += 1) {
        ZASSERT(int(f.fail(id, ReindexQueue::Failure::Preempted)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.requeues(id) == 0u);
    ZASSERT(f.queue.pending_reason(id));
}

ZEST_CASE(OwnCrashGivesUp) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/poison.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);

    // The run named by its dying worker would crash the next one too: the
    // file waits for its content to change.
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Crashed)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.queue.pending_reason(id).has_value());
}

ZEST_CASE(LostSpendsBudget) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/poison.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);

    for(unsigned i = 0; i < IndexerFixture::budget; i += 1) {
        ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) ==
                int(IndexerFixture::Decision::Requeued));
    }
    ZASSERT(f.requeues(id) == IndexerFixture::budget);

    // A preemption still requeues a file whose budget is spent: dropping
    // it would erase the pending state and serve the stale shard as fresh.
    // Only the next lost run gives up.
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Preempted)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.requeues(id) == IndexerFixture::budget);

    // Giving up clears the pending reindex: nothing is left to requeue, and
    // the stale shard serves as fresh — the accepted cost of abandoning.
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) == int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.queue.pending_reason(id).has_value());
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(StaleCrashKeepsBudget) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/edited.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    auto stale = f.seq(id);

    // The user fixes the file while the old bytes' dispatch is in flight:
    // the stale crash must not spend the fixed content's budget or touch
    // its pending reindex.
    f.background.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(int(f.fail_at(id, stale, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::Outdated));
    ZASSERT(f.requeues(id) == 0u);
    ZASSERT(f.queue.pending_reason(id));
}

ZEST_CASE(DepsDowngradeKeepsContent) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/c.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.seq(id);

    // The content pass is dispatched; a deps-only cascade arrives mid-flight
    // and downgrades the pending reason, betting on that pass to cover the
    // edit. The pass fails — the requeue must restore the ContentChanged
    // reindex or the stale shard stops being suppressed.
    f.consume(id);
    f.background.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(*f.queue.pending_reason(id)) == int(ReindexReason::DepsOnly));

    ZASSERT(int(f.fail_at(id, launch, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(*f.queue.pending_reason(id)) == int(ReindexReason::ContentChanged));
    ZASSERT(f.requeues(id) == 1u);
}

ZEST_CASE(GaveUpClearsDowngraded) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/d.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.seq(id);
    f.set_requeues(id, IndexerFixture::budget);

    // A deps-only enqueue arrives mid-flight, then the content pass spends
    // its last life. The downgraded entry must not stay queued: its retry
    // is doomed, and the give-up already accepted the staleness.
    f.consume(id);
    f.background.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(int(f.fail_at(id, launch, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::GaveUp));
    ZASSERT(!f.queue.pending_reason(id).has_value());
}

ZEST_CASE(OnlyUnitsBooked) {
    // A file the build does not compile as a unit is never booked, whoever
    // asks: no attempt would ever finish for it.
    IndexerFixture f;
    auto header = f.project.file_table.intern(Spelling::absolute("/fake/a.h"));
    auto unlisted = f.project.file_table.intern(Spelling::absolute("/fake/gone.cpp"));
    auto unit = list_unit(f.project.cdb, "/fake/a.cpp");

    ZEXPECT(!f.background.enqueue(header, ReindexReason::ContentChanged));
    ZEXPECT(!f.background.enqueue(unlisted, ReindexReason::DepsOnly));
    ZEXPECT(f.background.enqueue(unit, ReindexReason::DepsOnly));
    ZEXPECT(IndexerFixture::pending_files(f.queue) == llvm::SmallVector<Fid>{unit});
}

ZEST_CASE(StartBooksOwed) {
    // Before its owner starts it the index runs no pass, and what is booked
    // waits. The start books what the index owes — every member it holds
    // nothing of and every outdated unit, DepsOnly, every unit owed the
    // variants a rebuild discarded, ContentChanged — and runs the pass.
    IndexerFixture f;
    f.project.settings.project.idle_timeout_ms = 0;
    auto unindexed = list_unit(f.project.cdb, "/fake/unindexed.cpp");
    auto outdated = list_unit(f.project.cdb, "/fake/outdated.cpp");
    auto owed = list_unit(f.project.cdb, "/fake/owed.cpp");
    auto current = list_unit(f.project.cdb, "/fake/current.cpp");
    auto edited = list_unit(f.project.cdb, "/fake/edited.cpp");
    for(auto unit: {outdated, owed, current}) {
        index::TUManifest manifest;
        manifest.built_at = 1;
        manifest.tu_version = f.project.file_table.intern_version(unit, 1);
        f.project.index.apply_manifest(unit, std::move(manifest));
    }
    f.project.index.mark_outdated(outdated);
    IndexInternals::owe(f.project.index, owed);

    f.background.enqueue(edited, ReindexReason::ContentChanged);
    f.background.schedule();
    f.loop.run();
    ZEXPECT(f.background.idle());
    ZEXPECT(f.failures() == 0u);
    ZEXPECT(IndexerFixture::pending_files(f.queue) == llvm::SmallVector<Fid>{edited});

    f.background.start();
    ZEXPECT(f.queue.pending_reason(unindexed) == ReindexReason::DepsOnly);
    ZEXPECT(f.queue.pending_reason(outdated) == ReindexReason::DepsOnly);
    ZEXPECT(f.queue.pending_reason(owed) == ReindexReason::ContentChanged);
    ZEXPECT(f.queue.pending_reason(edited) == ReindexReason::ContentChanged);
    ZEXPECT(!f.queue.pending_reason(current).has_value());
    ZEXPECT(!f.background.idle());

    // No worker runs here: each booked unit's run fails.
    f.loop.run();
    ZEXPECT(f.background.idle());
    ZEXPECT(f.failures() == 4u);
}

ZEST_CASE(ShutdownEndsPasses) {
    // Once the project shut down, its background index runs no pass again:
    // what was booked stays booked for the next session, and nothing is
    // owed in this one.
    IndexerFixture f;
    f.project.settings.project.idle_timeout_ms = 60'000;
    auto unit = list_unit(f.project.cdb, "/fake/a.cpp");
    f.background.start();
    f.background.enqueue(unit, ReindexReason::ContentChanged);
    auto task = f.project.shutdown();
    f.loop.schedule(task);
    f.loop.run();
    ZEXPECT(f.queue.pending_reason(unit) == ReindexReason::ContentChanged);
    ZEXPECT(f.background.idle());

    f.project.settings.project.idle_timeout_ms = 0;
    f.background.schedule();
    f.background.schedule(/*immediate=*/true);
    f.loop.run();
    ZEXPECT(f.queue.pending_reason(unit) == ReindexReason::ContentChanged);
    ZEXPECT(f.failures() == 0u);
    ZEXPECT(f.background.idle());
}

ZEST_CASE(DroppedWithoutPending) {
    IndexerFixture f;
    auto id = f.project.file_table.intern(Spelling::absolute("/proj/gone.cpp"));
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) == int(IndexerFixture::Decision::Dropped));
}

ZEST_CASE(AttemptWaitPerSeq) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/waited.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    auto launch = f.seq(id);

    bool first_woke = false;
    auto first_body = [&]() -> async::task<> {
        co_await f.background.await_attempt(id);
        first_woke = true;
    };
    auto first = first_body();
    f.loop.schedule(first);
    f.loop.run();

    // A requeue arrives mid-flight: the entry stays pending under a fresh
    // sequence number, and a new waiter binds to that newer attempt.
    f.consume(id);
    f.background.enqueue(id, ReindexReason::DepsOnly);
    bool second_woke = false;
    auto second_body = [&]() -> async::task<> {
        co_await f.background.await_attempt(id);
        second_woke = true;
    };
    auto second = second_body();
    f.loop.schedule(second);
    f.loop.run();

    // The flight's completion covers the sequence number its waiter observed, no
    // matter the requeue: await_attempt promises one attempt, and holding
    // the waiter through every follow-up would park a feature request for
    // as long as edits keep arriving.
    f.wake_waiters(id, launch);
    ZASSERT(first_woke);
    ZASSERT(!second_woke);

    f.wake_waiters(id, f.seq(id));
    ZASSERT(second_woke);
}

ZEST_CASE(ContentChangeResetsBudget) {
    IndexerFixture f;
    auto id = list_unit(f.project.cdb, "/proj/fixed.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);

    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    ZASSERT(f.requeues(id) == 2u);

    // The user fixes the file: new content starts a fresh poison budget.
    f.background.enqueue(id, ReindexReason::ContentChanged);
    ZASSERT(f.requeues(id) == 0u);

    // A deps-only cascade is not new content and keeps the attempt count.
    ZASSERT(int(f.fail(id, ReindexQueue::Failure::Lost)) ==
            int(IndexerFixture::Decision::Requeued));
    f.background.enqueue(id, ReindexReason::DepsOnly);
    ZASSERT(f.requeues(id) == 1u);
}

ZEST_CASE(PassSnapshotBoundary) {
    // Manual passes of a background index never started: the tail
    // schedule() no-ops, so the boundary between the two passes stays
    // observable.
    IndexerFixture f;

    auto a = list_unit(f.project.cdb, "/fake/a.cpp");
    auto b = list_unit(f.project.cdb, "/fake/b.cpp");
    auto c = list_unit(f.project.cdb, "/fake/c.cpp");

    f.background.enqueue(a, ReindexReason::ContentChanged);
    f.background.enqueue(b, ReindexReason::ContentChanged);

    // Grow the queue from inside the pass: the first Report enqueues a
    // third file, which must arrive past the pass snapshot and wait for the
    // next pass instead of being consumed by this one.
    bool grew = false;
    BackgroundIndex::Progress first_end;
    auto conn = f.background.on_progress_changed.connect([&] {
        auto& progress = f.background.progress();
        if(progress.stage == BackgroundIndex::Progress::Stage::Report && !grew) {
            grew = true;
            f.background.enqueue(c, ReindexReason::ContentChanged);
        }
        if(progress.stage == BackgroundIndex::Progress::Stage::End && first_end.total == 0) {
            first_end = progress;
        }
    });

    f.run_pass();

    ZASSERT(grew);
    ZASSERT(first_end.total == 2u);
    ZASSERT(first_end.dispatched == 2u);
    ZASSERT(first_end.completed == 2u);
    ZASSERT(f.queue.queued() == 1u);

    f.run_pass();

    ZASSERT(f.queue.queued() == 0u);
    ZASSERT(f.failures() == 3u);
    ZASSERT(f.background.idle());
}

ZEST_CASE(PauseResumesPass) {
    IndexerFixture f;

    auto a = list_unit(f.project.cdb, "/fake/a.cpp");
    auto b = list_unit(f.project.cdb, "/fake/b.cpp");
    f.background.enqueue(a, ReindexReason::ContentChanged);
    f.background.enqueue(b, ReindexReason::ContentChanged);

    // Pause from inside the pass (the first Report), resume from a
    // separately scheduled task: the feeder must park on the resume event
    // and drain the rest of the pass afterwards.
    bool paused = false;
    auto conn = f.background.on_progress_changed.connect([&] {
        if(f.background.progress().stage == BackgroundIndex::Progress::Stage::Report && !paused) {
            paused = true;
            f.background.pause();
        }
    });

    auto resume_body = [&]() -> async::task<> {
        co_await async::yield();
        f.background.resume();
    };
    auto pass = f.pass_task();
    auto resumer = resume_body();
    f.loop.schedule(pass);
    f.loop.schedule(resumer);
    f.loop.run();

    ZASSERT(paused);
    ZASSERT(f.queue.queued() == 0u);
    ZASSERT(f.failures() == 2u);
    ZASSERT(f.background.idle());
}

};  // ZEST_SUITE(BackgroundIndexRequeue)

/// The index's serving-neutral change reports and the background index
/// taking them: every index-changing source reports the reindexes it owes
/// and the index changes.
ZEST_SUITE(BackgroundIndexReports) {

ZEST_CASE(MergeReportsIndexChanged) {
    IndexerFixture f;
    TempDir tmp;
    tmp.touch("main.cpp", "int value() { return 1; }\n");
    auto indexed = index_file(tmp, tmp.path("main.cpp"));
    ZASSERT(!indexed.data.empty());

    ZASSERT(f.merge(indexed.data));

    ZASSERT(ranges::contains(f.changed,
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

    f.changed.clear();
    index::IndexReport report;
    f.project.index.drop(tu_id, report);
    ZASSERT(ranges::contains(report.index_changed(), tu_id));
    ZASSERT(ranges::contains(report.index_changed(), header_id));

    f.project.take_report(report);
    ZASSERT(ranges::contains(f.changed, header_id));
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

    f.changed.clear();
    f.save();

    ZASSERT(!f.project.index.shard(header_id));
    ZASSERT(ranges::contains(f.changed, header_id));
}

ZEST_CASE(IndexFirstRearmsIdleTimer) {
    // s#9: an index_first colliding with an already-armed idle timer must re-arm
    // it to fire now. Un-fixed, this test waits out the full idle window
    // below instead of finishing promptly.
    IndexerFixture f;
    f.project.settings.project.idle_timeout_ms = 60'000;

    auto id = list_unit(f.project.cdb, "/fake/a.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    f.background.start();
    f.background.index_first(id);
    f.loop.run();

    ZASSERT(f.background.idle());
    ZASSERT(f.failures() == 1u);
}

ZEST_CASE(OtherCommandNotSkipped) {
    // The skip check runs a unit whose index is current by content but was
    // built under another command than the build gives it — as a run that
    // started under the old command merges it.
    TempDir tmp;
    tmp.touch("same.cpp", "int same() { return 1; }\n");
    tmp.touch("moved.cpp", "int moved() { return 2; }\n");
    IndexerFixture f;
    f.project.cdb.add_command(tmp.root,
                              tmp.path("same.cpp"),
                              llvm::StringRef("clang++ -c same.cpp"));
    f.project.cdb.add_command(tmp.root,
                              tmp.path("moved.cpp"),
                              llvm::StringRef("clang++ -c moved.cpp"));
    auto same = f.project.file_table.intern(Spelling::absolute(tmp.path("same.cpp")));
    auto moved = f.project.file_table.intern(Spelling::absolute(tmp.path("moved.cpp")));
    ZASSERT(f.merge(index_file(tmp, tmp.path("same.cpp")).data));
    ZASSERT(f.project.index.merge(index_file(tmp, tmp.path("moved.cpp")).data,
                                  {},
                                  f.project.command_identity(moved) + 1));

    f.background.enqueue(same, ReindexReason::DepsOnly);
    f.background.enqueue(moved, ReindexReason::DepsOnly);
    f.run_pass();

    // No worker runs here: the run fails, the skip does not.
    ZEXPECT(!f.background.failed(same));
    ZEXPECT(f.background.failed(moved));
}

ZEST_CASE(SaveStartsOwedPass) {
    // A save outside any pass that finds the database corrupt owes every
    // unit a reindex: their pass starts, rather than waiting for the next
    // edit or checkpoint.
    TempDir tmp;
    tmp.touch("main.cpp", "int use() { return 0; }\n");
    IndexerFixture f;
    open_store(tmp, f.project);
    f.project.settings.project.idle_timeout_ms = 0;
    // The index started long before: its first pass is over.
    f.background.start();
    f.loop.run();
    f.project.database = std::make_unique<FailingDatabase>(std::move(f.project.database), true);
    f.list_units(tmp, {{"main.cpp"}});
    ZASSERT(f.merge(index_file(tmp, tmp.path("main.cpp")).data));

    auto task = f.project.save();
    f.loop.schedule(task);
    f.loop.run();

    ZASSERT(f.background.idle());
    ZEXPECT(f.failures() == 1u);
}

ZEST_CASE(SaveKeepsQueuedSchedule) {
    // A reindex an edit queued waits out the idle window its own schedule
    // armed: a save meanwhile owes it nothing.
    IndexerFixture f;
    // The index started: its first pass found nothing to index.
    f.project.settings.project.idle_timeout_ms = 0;
    f.background.start();
    f.loop.run();
    f.project.settings.project.idle_timeout_ms = 60'000;

    auto id = list_unit(f.project.cdb, "/fake/a.cpp");
    f.background.enqueue(id, ReindexReason::ContentChanged);
    auto task = f.project.save();
    f.loop.schedule(task);
    f.loop.run();

    ZEXPECT(f.background.queue().queued() == 1u);
    ZEXPECT(f.failures() == 0u);
}

ZEST_CASE(IndexFirstSparesRunningPass) {
    // A file put first once the running pass took it — in flight, or done
    // as here — is queued past the pass and leads the next one. Moved into
    // the running pass, it would run there a second time and push the
    // pass's last file out.
    IndexerFixture f;
    f.project.settings.project.idle_timeout_ms = 0;

    llvm::SmallVector<Fid> files;
    for(auto name: {"/fake/a.cpp", "/fake/b.cpp", "/fake/c.cpp"}) {
        files.push_back(list_unit(f.project.cdb, name));
        f.background.enqueue(files.back(), ReindexReason::DepsOnly);
    }

    // The index_first arrives as the first file's attempt ends.
    std::vector<llvm::SmallVector<Fid>> passes(1);
    std::vector<std::size_t> completed;
    auto finished = f.background.on_attempt_finished.connect([&](Fid id) {
        if(passes.size() == 1 && passes.front().empty()) {
            f.background.index_first(id);
        }
        passes.back().push_back(id);
    });
    auto conn = f.background.on_progress_changed.connect([&] {
        if(f.background.progress().stage == BackgroundIndex::Progress::Stage::End) {
            completed.push_back(f.background.progress().completed);
            passes.emplace_back();
        }
    });
    f.background.start();
    f.loop.run();

    ZEXPECT(passes.size() == 3u);
    ZEXPECT(completed == std::vector<std::size_t>{3, 1});
    ZEXPECT(passes[0] == files);
    ZEXPECT(passes[1] == llvm::SmallVector<Fid>{files[0]});
    ZEXPECT(f.background.idle());
}

ZEST_CASE(EmptyPassBuildsSearch) {
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

    f.run_pass();
    ZEXPECT(f.project.index.search_index().loaded());
}

ZEST_CASE(EmptyPassDiscardsDamage) {
    // A scan met a damaged page after the load: a session finding nothing
    // to index, its search index current, still discards the database.
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
    f.save(index::SearchRebuild::Idle);
    ZASSERT(!f.project.index.search_update_due(index::SearchRebuild::Idle));
    ZASSERT(!f.project.index.damaged());

    page.damaged = true;
    ZEXPECT(f.project.index.damaged());
    f.run_pass();
    // The recovery replaced the database, the spy with it.
    ZEXPECT(condemned);
}

};  // ZEST_SUITE(BackgroundIndexReports)

}  // namespace
}  // namespace clice::testing
