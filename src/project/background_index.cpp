module clice;

import :index.project_index;
import :project.background_index;
import :project.project;
import :project.save;
import :project.unit_run;
import :support.environment;
import :support.logging;
import :support.process;
import :support.timer;
import :worker.pool;
import :worker.protocol;

namespace clice {

namespace {

/// A round's shared counters, living on run_round's frame, which outlives
/// every spawned task (it joins them before returning).
struct RoundState {
    /// Dispatched tasks not yet finished.
    std::size_t inflight = 0;

    /// Set whenever a task finishes, waking a feeder waiting out the cap.
    async::event task_done{false};
};

void record_indexed(BackgroundIndex& background,
                    Fid file,
                    std::vector<worker::LocatedMessage> compile_errors) {
    background.failed.erase(file);
    background.too_big.erase(file);
    if(compile_errors.empty()) {
        background.errors.erase(file);
    } else {
        background.errors[file] = std::move(compile_errors);
    }
}

void record_failed(BackgroundIndex& background, Fid file) {
    background.failed.insert(file);
    background.too_big.erase(file);
    background.errors.erase(file);
}

void record_too_big(BackgroundIndex& background, Fid file, std::uint64_t memory) {
    record_failed(background, file);
    background.too_big[file] = memory;
}

/// One dispatched unit of a background round: admit, run the TU through
/// its unit-run node, take the merge report, finish the taken reindex and
/// report progress.
async::task<> run_index_task(Project& project,
                             ReindexQueue::TakenReindex taken,
                             std::size_t index,
                             std::size_t total,
                             RoundState& round) {
    using Result = UnitRunOutcome::Result;
    auto& background = project.background_index;
    auto& queue = background.queue;
    auto path_id = taken.id;
    auto file_path = std::string(project.file_table.resolve(path_id));
    // The engine's own observation is authoritative for content
    // changes: it saw the event. The dep-hash check cannot be trusted
    // to see a file's own edit (it validates the recorded
    // dependencies), so only deps-only slots — where it exists to
    // deduplicate cascade storms — may take the shortcut.
    bool content_changed = queue.pending_reason(path_id) == ReindexReason::ContentChanged;
    bool served = !content_changed && background.compiled_by_session &&
                  background.compiled_by_session(path_id);
    bool skipped = background.on_skipped_diagnostics && project.settings.project.skip_header_bodies;
    if(!served && (content_changed || (skipped && background.lacks_skipped_diagnostics(path_id)) ||
                   project.index.needs_reindex(path_id))) {
        LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);
        auto outcome =
            co_await run_unit(project,
                              path_id,
                              {.index = true,
                               .errors = background.errors_wanted,
                               .skipped_diagnostics = skipped,
                               .outdated = [&queue, taken] { return queue.outdated(taken); }});
        if(outcome.result == Result::Shutdown) {
            // The graph refused the round: nothing ran, so the reindex
            // stays booked for the final snapshot and the next session;
            // only the round bookkeeping below still runs so a live feeder
            // is not left waiting on this slot.
            background.progress.completed += 1;
            round.inflight -= 1;
            round.task_done.set();
            co_return;
        }
        // The report's reindexes are booked before the finish and the
        // waiter wake-ups below: a waker re-deriving its route must never
        // observe an index as current that the merge just declared stale.
        apply_report(project, outcome.report);
        switch(outcome.result) {
            case Result::Completed: {
                record_indexed(background, path_id, std::move(outcome.errors));
                if(auto pending = queue.peek(path_id);
                   skipped && pending && pending->seq == taken.seq) {
                    background.on_skipped_diagnostics(path_id,
                                                      outcome.text_hash,
                                                      std::move(outcome.skipped_diagnostics));
                }
                background.indexed += 1;
                LOG_PERF("index",
                         "progress={}/{} file={} bytes={} index_ms={} merge_ms={}",
                         index,
                         total,
                         file_path,
                         outcome.perf.bytes,
                         outcome.perf.index_ms,
                         outcome.perf.merge_ms);
                break;
            }
            case Result::Skipped: {
                break;
            }
            case Result::Failed: {
                LOG_WARN("[{}/{}] Index failed for {}: {}", index, total, file_path, outcome.error);
                record_failed(background, path_id);
                break;
            }
            case Result::Crashed:
            case Result::TooBig:
            case Result::Lost:
            case Result::Preempted:
            case Result::OutOfMemory: {
                // Preempted, killed for memory beside other work or lost to
                // a worker death: the work itself is fine — requeue the
                // file with its original reason so the next round redoes it
                // instead of silently dropping coverage; only lost runs
                // spend the bounded budget. A run that crashed its worker,
                // or that ran out of memory alone, waits for the file to
                // change: the same bytes would again.
                using enum ReindexQueue::Failure;
                auto failure = outcome.result == Result::Crashed || outcome.result == Result::TooBig
                                   ? Crashed
                               : outcome.result == Result::Lost ? Lost
                                                                : Preempted;
                switch(queue.on_dispatch_failure(taken, failure)) {
                    case ReindexQueue::FailureDecision::Dropped: {
                        LOG_INFO("[{}/{}] Index dropped for removed file {}",
                                 index,
                                 total,
                                 file_path);
                        break;
                    }
                    case ReindexQueue::FailureDecision::Outdated: {
                        LOG_INFO("[{}/{}] Index failure for outdated content of {}",
                                 index,
                                 total,
                                 file_path);
                        break;
                    }
                    case ReindexQueue::FailureDecision::GaveUp: {
                        // Giving up cleared the entry; no further attempt
                        // will come, so no waiter is left waiting for one.
                        wake_attempt_waits(project, path_id);
                        // Log-only by design: the file is usually not
                        // open (open documents are served by their
                        // session, not the shard), so there is no
                        // diagnostic surface. Cross-file references
                        // into this file stay stale until its content
                        // changes.
                        if(outcome.result == Result::TooBig) {
                            LOG_WARN(
                                "[{}/{}] {} does not fit in memory: its worker reached {} MiB "
                                "alone; its cross-file data stays stale until it changes",
                                index,
                                total,
                                file_path,
                                outcome.memory >> 20);
                            record_too_big(background, path_id, outcome.memory);
                            break;
                        }
                        LOG_WARN(
                            "[{}/{}] Index giving up on {} ({}); its cross-file data stays "
                            "stale until it changes",
                            index,
                            total,
                            file_path,
                            outcome.error);
                        record_failed(background, path_id);
                        break;
                    }
                    case ReindexQueue::FailureDecision::Requeued: {
                        if(failure == Lost) {
                            LOG_WARN("[{}/{}] Worker died while indexing {}; requeued: {}",
                                     index,
                                     total,
                                     file_path,
                                     outcome.error);
                        } else {
                            LOG_INFO("[{}/{}] Index requeued for {}: {}",
                                     index,
                                     total,
                                     file_path,
                                     outcome.error);
                        }
                        break;
                    }
                }
                break;
            }
            case Result::Shutdown: {
                std::unreachable();
            }
        }
    }
    // The pending window ends with the index attempt, success or not. On
    // failure the last-known index resumes serving — deliberately: keeping
    // the gate would hide a file that fails to index (broken compile,
    // missing command) from every cross-file query with no recovery path,
    // since only a future event re-enqueues it. Any such event re-judges
    // staleness by content hash. A re-enqueue during the flight booked
    // a newer reindex: the finish leaves it standing.
    queue.finish(taken);
    // The attempt finished with no retry pending; the serving side judges
    // whether an open session still waiting on the index can ever be
    // served by it, before its waiters wake (contract 15).
    if(background.on_attempt_finished && !queue.contains(path_id)) {
        background.on_attempt_finished(path_id);
    }
    // Wake the waiters parked on this attempt (and older sequence numbers it
    // covers) after the escalation above, so they re-derive their route
    // against the finished state. Waiters of a requeue made during the
    // flight bound the fresh sequence number and stay parked for that newer
    // attempt.
    wake_attempt_waits(project, path_id, taken.seq);
    round.inflight -= 1;
    round.task_done.set();
    background.progress.stage = BackgroundIndex::Progress::Stage::Report;
    background.progress.completed += 1;
    background.on_progress_changed.emit();
}

/// The round's dispatch loop, spawned as a child of `workers` so that a
/// shutdown cancel reaches it through the round frame's join — see the
/// spawn site. Takes the round's slots and spawns one run_index_task per
/// live slot, bounded by the feeder window.
async::task<> run_round_feeder(Project& project,
                               async::task_group<>& workers,
                               RoundState& round,
                               std::size_t total,
                               std::size_t& dispatched) {
    auto& background = project.background_index;
    auto& pool = *project.pool;
    while(background.queue.round_left()) {
        // Every wait loops back to re-check ALL gates: a pause arriving
        // while parked on capacity (or a capacity loss while parked on the
        // pause) must not let one slot slip through on wake-up.
        while(true) {
            if(background.pause_depth > 0) {
                co_await background.resume_event.wait();
                continue;
            }

            // With no schedulable worker but one returning, a dispatch would
            // only convert the queue slot into an instant worker_unavailable
            // failure; park until a slot returns to service. With none ever
            // returning such failures are terminal and take the normal
            // failure path.
            if(pool.schedulable_stateless() == 0 && co_await pool.await_capacity(false)) {
                continue;
            }

            // Feed at most twice the pool's low budget: deep enough that
            // workers never idle waiting for the feeder, shallow enough
            // that the pool queue holds little when a pause or budget cut
            // arrives.
            if(round.inflight >= 2 * pool.background_limit()) {
                round.task_done.reset();
                co_await round.task_done.wait();
                continue;
            }
            break;
        }

        // No open-session or hash-freshness shortcut here: the index task
        // is the single decision point for skipping (it knows the pending
        // reason; a hash check alone cannot see a file's own edit), and
        // its finish consumes the taken reindex with newer recordings
        // honored. A second, reason-blind copy of these checks here is
        // exactly what once erased ContentChanged state early and let a
        // stale shard keep serving.
        auto taken = background.queue.take_next();
        if(!taken) {
            break;
        }

        dispatched += 1;
        round.inflight += 1;
        if(background.dispatches_before_pause > 0) {
            background.dispatches_before_pause -= 1;
            if(background.dispatches_before_pause == 0) {
                pause_indexing(project);
            }
        }
        // A free coroutine, not an immediately-invoked capturing lambda: a
        // lambda's captures live in the lambda object, which dies at the
        // end of this statement — anything read after the first suspension
        // would dangle. Coroutine parameters are copied into the frame.
        workers.spawn(run_index_task(project, *taken, dispatched, total, round));
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
}

}  // namespace

void wake_attempt_waits(Project& project, Fid path_id, std::uint64_t seq) {
    auto& background = project.background_index;
    auto it = background.attempt_waits.find(path_id);
    if(it == background.attempt_waits.end()) {
        return;
    }
    llvm::SmallVector<std::shared_ptr<async::event>, 2> woken;
    for(auto& wait: it->second) {
        if(wait.seq <= seq) {
            woken.push_back(std::move(wait.event));
        }
    }
    it->second.erase(
        ranges::remove_if(it->second,
                          [&](const BackgroundIndex::AttemptWait& wait) { return wait.seq <= seq; })
            .begin(),
        it->second.end());
    if(it->second.empty()) {
        background.attempt_waits.erase(it);
    }
    for(auto& event: woken) {
        event->set();
    }
}

async::task<> run_index_round(Project& project) {
    auto& background = project.background_index;
    auto& queue = background.queue;
    if(background.idle_timer) {
        co_await background.idle_timer->wait();
    }
    background.scheduled = false;

    if(queue.queued() == 0) {
        LOG_DEBUG("Background indexing: queue exhausted");
        // Nothing to index, yet the search index may be owed the full build
        // a previous session never reached — until it runs, every search
        // scans the symbol records — or a run of the entries it left
        // pending, or the start-up scan met a damaged record, which only a
        // save discards (a damaged database the save discarded queues
        // every unit, and the save starts their round).
        if(project.index.damaged() || project.index.search_update_due(index::SearchRebuild::Idle)) {
            co_await persist(project, index::SearchRebuild::Idle);
        }
        co_return;
    }

    background.active = true;
    LOG_DEBUG("Background indexing: starting, {} files queued", queue.queued());

    // FileVersion results hold for one round: the disk can change under a
    // running round, but staleness is re-judged per round anyway.
    project.index.begin_round();

    auto total =
        queue.begin_round([&](Fid id) { return !project.dep_graph.module_of(id).empty(); });
    std::size_t dispatched = 0;
    RoundState round;

    // Announce the round; a progress reporter reads the counts from
    // `progress` and owns the LSP token's begin/report/end handshake. With
    // no subscriber the signal is simply a no-op.
    background.progress = {.stage = BackgroundIndex::Progress::Stage::Begin, .total = total};
    background.on_progress_changed.emit();

    // Timed at the start of real work; the reporter's token handshake runs
    // off to the side and cannot inflate the reported indexing duration.
    ScopedTimer timer;
    // The dispatch loop runs as the first child of the group it fills, so a
    // shutdown cancel reaches the feeder and every in-flight task alike.
    co_await async::with_task_group([&](async::task_group<>& workers) {
        return run_round_feeder(project, workers, round, total, dispatched);
    });

    background.progress.stage = BackgroundIndex::Progress::Stage::End;
    background.progress.dispatched = dispatched;
    background.on_progress_changed.emit();

    // No dispatch loop holds a slot of the queue any more.
    queue.end_round();

    LOG_PERF("index",
             "phase=run dispatched={} skipped={} total={} elapsed_ms={}",
             dispatched,
             total - dispatched,
             total,
             timer.ms());
    co_await persist(project,
                     queue.queued() == 0 ? index::SearchRebuild::Idle
                                         : index::SearchRebuild::Behind);
    // The round's merge buffers are freed by now; glibc would keep their
    // pages, and the master its round's peak, for the rest of the session.
    co_await async::queue([] { release_free_memory(); });

    // The round owns the "active" gate through its save: releasing it
    // before the write await would let a next round's save overlap this
    // one's in-flight batch, racing same-key blob writes on the pool.
    background.active = false;
    background.round_ended.set();

    // Files enqueued or requeued while the round ran saw their schedule
    // no-op against `active`; without this kick they would wait for the
    // next external event — and a content-changed pending file's index
    // stays skipped for that whole wait.
    if(queue.queued() > 0) {
        schedule_index(project, /*immediate=*/true);
    }
}

bool enqueue_reindex(Project& project, Fid path_id, ReindexReason reason) {
    // The one admission point of the background index: a rule's
    // `index = false` keeps its files out here, whichever path asked.
    if(!project.build.indexed(project.file_table.resolve(path_id))) {
        return false;
    }
    // A new pending reindex voids the running round's freshness memos: one
    // taken after this recording must re-judge against the disk, or its
    // skip would finish the fresh sequence number on a result memoized
    // before the change and the file would never re-index.
    project.index.begin_round();
    project.background_index.queue.record(path_id, reason);
    return true;
}

void index_first(Project& project, Fid path_id) {
    if(!enqueue_reindex(project, path_id, ReindexReason::DepsOnly)) {
        // No attempt will ever finish for an excluded file: the serving
        // side hears that now, or the session waits on the index forever.
        if(auto& finished = project.background_index.on_attempt_finished) {
            finished(path_id);
        }
        return;
    }
    // The file someone is reading beats the bulk backlog.
    project.background_index.queue.move_to_front(path_id);
    schedule_index(project, /*immediate=*/true);
}

void enqueue_outdated(Project& project, llvm::ArrayRef<Fid> members) {
    auto stale = project.index.stale_units();
    if(!project.settings.project.enable_indexing.value) {
        return;
    }
    for(auto member: members) {
        if(!project.index.indexed(member)) {
            enqueue_reindex(project, member, ReindexReason::DepsOnly);
        }
    }
    for(auto unit: stale) {
        enqueue_reindex(project, unit, ReindexReason::DepsOnly);
    }
    for(auto unit: project.index.owed_units()) {
        enqueue_reindex(project, unit, ReindexReason::ContentChanged);
    }
    schedule_index(project);
}

void apply_report(Project& project, const index::IndexReport& report) {
    for(auto id: report.reindex()) {
        enqueue_reindex(project, id, ReindexReason::ContentChanged);
    }
    if(!report.index_changed().empty()) {
        project.background_index.on_index_changed.emit(report.index_changed());
    }
}

llvm::SmallVector<Fid> save_debt(const Project& project) {
    auto& background = project.background_index;
    llvm::SmallVector<Fid> debt(background.failed.begin(), background.failed.end());
    auto pending = background.queue.pending_files();
    debt.append(pending.begin(), pending.end());
    return debt;
}

async::task<> persist(Project& project, index::SearchRebuild search) {
    auto& queue = project.background_index.queue;
    bool retry = true;
    while(retry) {
        auto report = co_await save_project(project, *project.loop, save_debt(project), search);
        retry = report.owes_retry;
        apply_report(project, report);
        // No more indexing is queued, so no save is due for a while: the
        // file may shrink — unless a file was queued while the save ran. A
        // shrink that discarded the database owes the fresh one a save.
        if(!retry && search == index::SearchRebuild::Idle && queue.queued() == 0) {
            report = co_await shrink_database(project, *project.loop);
            retry = report.owes_retry;
            apply_report(project, report);
        }
    }
    // A save the compaction or a discarded database owed reindexes to queued
    // them: they need a round of their own when none is running.
    if(queue.queued() > 0) {
        schedule_index(project, /*immediate=*/true);
    }
}

async::task<> await_index_attempt(Project& project, Fid path_id) {
    auto pending = project.background_index.queue.peek(path_id);
    if(!pending) {
        co_return;
    }
    auto seq = pending->seq;
    auto& waits = project.background_index.attempt_waits[path_id];
    auto it = ranges::find(waits, seq, &BackgroundIndex::AttemptWait::seq);
    if(it == waits.end()) {
        waits.push_back({seq, std::make_shared<async::event>()});
        it = waits.end() - 1;
    }
    // Hold the shared_ptr across the await: the wake moves the event out
    // of the map before setting it, and a fresh wait after that parks on a
    // new instance.
    auto event = it->event;
    co_await event->wait();
}

void clear_reindex(Project& project, Fid path_id) {
    project.background_index.queue.clear(path_id);
    wake_attempt_waits(project, path_id);
}

void pause_indexing(Project& project) {
    auto& background = project.background_index;
    background.pause_depth += 1;
    if(background.pause_depth == 1) {
        background.resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void pause_indexing_after(Project& project, std::size_t units) {
    project.background_index.dispatches_before_pause = units;
}

void resume_indexing(Project& project) {
    auto& background = project.background_index;
    if(background.pause_depth > 0) {
        background.pause_depth -= 1;
    }
    if(background.pause_depth == 0) {
        background.resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

async::task<> stop_background_index(Project& project) {
    auto& background = project.background_index;
    background.stopped = true;
    background.tasks.cancel();
    co_await background.tasks.join();
    // Cancelling the round that waited on the idle timer leaves it armed,
    // and an armed timer keeps the loop running until it fires.
    if(background.idle_timer) {
        background.idle_timer->stop();
    }
    // Cancelled tasks unwind before their finish bookkeeping; release any
    // parked feature request rather than stranding it past shutdown.
    for(auto& [path_id, waits]: background.attempt_waits) {
        for(auto& wait: waits) {
            wait.event->set();
        }
    }
    background.attempt_waits.clear();
}

void schedule_index(Project& project, bool immediate) {
    auto& background = project.background_index;
    if(!project.settings.project.enable_indexing.value || background.active || background.stopped) {
        return;
    }
    if(background.scheduled) {
        // An immediate request colliding with an armed idle timer re-arms
        // it to fire now — dropped, a reader asking for index_first would
        // wait out the full idle window an earlier ordinary schedule
        // started (s#9).
        if(immediate && background.idle_timer) {
            background.idle_timer->start(chrono::milliseconds(0));
        }
        return;
    }
    background.scheduled = true;

    if(!background.idle_timer) {
        background.idle_timer = std::make_shared<async::timer>(async::timer::create(*project.loop));
    }
    // Time is the feature: the idle timeout batches edit storms into one
    // round. A follow-up round for work requeued during the round that just
    // ended has already been batched by that round and starts right away —
    // a crashed file's retry must not owe an extra idle window on top of
    // the round boundary it already waited out.
    background.idle_timer->start(
        chrono::milliseconds(immediate ? 0 : project.settings.project.idle_timeout_ms.value) +
        test_jitter());

    [[maybe_unused]] bool spawned = background.tasks.spawn(run_index_round(project));
    assert(spawned && "the background index's task group refused a round before it stopped");
}

bool index_idle(const Project& project) {
    auto& background = project.background_index;
    return !background.active && background.queue.queued() == 0;
}

async::task<> wait_index_idle(Project& project) {
    while(!index_idle(project)) {
        project.background_index.round_ended.reset();
        co_await project.background_index.round_ended.wait();
    }
}

chrono::milliseconds checkpoint_interval() {
    // Lets integration tests kill a server between checkpoints.
    if(auto ms = env_integer("CLICE_TEST_CHECKPOINT_MS")) {
        return chrono::milliseconds(*ms);
    }
    return chrono::minutes(5);
}

}  // namespace clice
