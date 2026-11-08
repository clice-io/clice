module clice;

import :index.project_index;
import :project.background;
import :project.graph.unit;
import :project.project;
import :support.environment;
import :support.logging;
import :support.process;
import :support.timer;
import :worker.pool;

namespace clice {

/// A pass's shared counters, living on run_pass's frame, which outlives
/// every spawned task (it joins them before returning).
struct BackgroundIndex::PassState {
    /// Dispatched tasks not yet finished.
    std::size_t inflight = 0;

    /// Set whenever a task finishes, waking a feeder waiting out the cap.
    async::event task_done{false};
};

/// One dispatched unit of a background pass: admit, compile the TU
/// through its unit node, finish the taken reindex and report progress.
async::task<> BackgroundIndex::run(ReindexQueue::TakenReindex taken,
                                   std::size_t index,
                                   std::size_t total,
                                   PassState& pass) {
    using Status = UnitResult::Status;
    auto path_id = taken.id;
    auto file_path = std::string(project.file_table.resolve(path_id));
    // A booked content change is authoritative: whoever booked it saw
    // the edit. The dep-hash check cannot be trusted
    // to see a file's own edit (it validates the recorded
    // dependencies), so only deps-only reindexes — where it exists to
    // deduplicate cascade storms — may take the shortcut.
    bool content_changed = reindexes.pending_reason(path_id) == ReindexReason::ContentChanged;
    bool diagnostics = reindexes.wants_diagnostics(path_id);
    auto compiled_by = project.compiled_by(path_id);
    if(content_changed ||
       (compiled_by != CompiledBy::Document && (diagnostics || !project.index_holds(path_id)))) {
        LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);
        // A build compiling the unit indexes it but brings no such
        // diagnostics: only a booking asking for them compiles it for them.
        bool wants_skipped =
            options.skipped_diagnostics && (compiled_by != CompiledBy::Build || diagnostics);
        auto result = co_await project.compile_unit(
            path_id,
            {.index = true,
             .errors = options.errors,
             .skipped_diagnostics = wants_skipped,
             .outdated = [this, taken] { return reindexes.outdated(taken); }});
        if(!result) {
            // The graph shut down: nothing ran, so the reindex
            // stays booked for the final snapshot and the next session;
            // only the pass bookkeeping below still runs so a live feeder
            // is not left waiting on this task.
            current.completed += 1;
            pass.inflight -= 1;
            pass.task_done.set();
            co_return;
        }
        // compile_unit booked the merge report's reindexes before the
        // finish and the waiter wake-ups below: a waker re-deriving its
        // route must never observe an index as current that the merge just
        // declared stale.
        switch(result->status) {
            case Status::Completed: {
                record(path_id, {.errors = std::move(result->errors)});
                // Unless a reindex booked during the run says they describe
                // inputs that changed.
                if(auto pending = reindexes.peek(path_id);
                   wants_skipped && pending && pending->seq == taken.seq) {
                    skipped[path_id] = {.text_hash = result->text_hash,
                                        .diagnostics = std::move(result->skipped_diagnostics)};
                }
                indexed_units += 1;
                LOG_PERF("index",
                         "progress={}/{} file={} bytes={} index_ms={} merge_ms={}",
                         index,
                         total,
                         file_path,
                         result->perf.bytes,
                         result->perf.index_ms,
                         result->perf.merge_ms);
                break;
            }
            case Status::Skipped: {
                break;
            }
            case Status::Failed: {
                LOG_WARN("[{}/{}] Index failed for {}: {}", index, total, file_path, result->error);
                record(path_id, {.failed = true});
                break;
            }
            case Status::Crashed:
            case Status::TooBig:
            case Status::Lost:
            case Status::Preempted:
            case Status::OutOfMemory: {
                // Preempted, killed for memory beside other work or lost to
                // a worker death: the work itself is fine — requeue the
                // file with its original reason so the next pass redoes it
                // instead of silently dropping coverage; only lost runs
                // spend the bounded budget. A run that crashed its worker,
                // or that ran out of memory alone, waits for the file to
                // change: the same bytes would again.
                using enum ReindexQueue::Failure;
                auto failure = result->status == Status::Crashed || result->status == Status::TooBig
                                   ? Crashed
                               : result->status == Status::Lost ? Lost
                                                                : Preempted;
                switch(reindexes.on_dispatch_failure(taken, failure)) {
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
                        wake_waiters(path_id);
                        // Log-only by design: the file is usually not
                        // open (open documents are served by their
                        // session, not the shard), so there is no
                        // diagnostic surface. Cross-file references
                        // into this file stay stale until its content
                        // changes.
                        if(result->status == Status::TooBig) {
                            LOG_WARN(
                                "[{}/{}] {} does not fit in memory: its worker reached {} MiB "
                                "alone; its cross-file data stays stale until it changes",
                                index,
                                total,
                                file_path,
                                result->memory >> 20);
                            record(path_id, {.failed = true, .too_big = result->memory});
                            break;
                        }
                        LOG_WARN(
                            "[{}/{}] Index giving up on {} ({}); its cross-file data stays "
                            "stale until it changes",
                            index,
                            total,
                            file_path,
                            result->error);
                        record(path_id, {.failed = true});
                        break;
                    }
                    case ReindexQueue::FailureDecision::Requeued: {
                        if(failure == Lost) {
                            LOG_WARN("[{}/{}] Worker died while indexing {}; requeued: {}",
                                     index,
                                     total,
                                     file_path,
                                     result->error);
                        } else {
                            LOG_INFO("[{}/{}] Index requeued for {}: {}",
                                     index,
                                     total,
                                     file_path,
                                     result->error);
                        }
                        break;
                    }
                }
                break;
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
    reindexes.finish(taken);
    if(!reindexes.contains(path_id)) {
        on_attempt_finished.emit(path_id);
    }
    // Wake the waiters parked on this attempt (and older sequence numbers it
    // covers) after what the attempt's end made of their document, so they
    // re-derive their route against the finished state. Waiters of a requeue made during the
    // flight bound the fresh sequence number and stay parked for that newer
    // attempt.
    wake_waiters(path_id, taken.seq);
    pass.inflight -= 1;
    pass.task_done.set();
    current.stage = Progress::Stage::Report;
    current.completed += 1;
    on_progress_changed.emit();
}

/// The pass's dispatch loop, spawned as a child of `workers` so that a
/// shutdown cancel reaches it through the pass frame's join — see the
/// spawn site. Takes the pass's files and spawns one run per live one,
/// bounded by the feeder window.
async::task<> BackgroundIndex::feed(async::task_group<>& workers,
                                    PassState& pass,
                                    std::size_t total,
                                    std::size_t& dispatched) {
    auto& pool = *project.pool;
    while(reindexes.pass_left()) {
        // Every wait loops back to re-check ALL gates: a pause arriving
        // while parked on capacity (or a capacity loss while parked on the
        // pause) must not let one file slip through on wake-up.
        while(true) {
            if(pause_depth > 0) {
                co_await resume_event.wait();
                continue;
            }

            // With no schedulable worker but one returning, a dispatch would
            // only convert the taken file into an instant worker_unavailable
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
            if(pass.inflight >= 2 * pool.background_limit()) {
                pass.task_done.reset();
                co_await pass.task_done.wait();
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
        auto taken = reindexes.take_next();
        if(!taken) {
            break;
        }

        dispatched += 1;
        pass.inflight += 1;
        if(dispatches_before_pause > 0) {
            dispatches_before_pause -= 1;
            if(dispatches_before_pause == 0) {
                pause();
            }
        }
        // A member coroutine, not an immediately-invoked capturing lambda: a
        // lambda's captures live in the lambda object, which dies at the
        // end of this statement — anything read after the first suspension
        // would dangle. Coroutine parameters are copied into the frame.
        workers.spawn(run(*taken, dispatched, total, pass));
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
}

void BackgroundIndex::record(Fid file, LastAttempt attempt) {
    if(!attempt.failed && attempt.errors.empty()) {
        attempts.erase(file);
    } else {
        attempts[file] = std::move(attempt);
    }
}

void BackgroundIndex::wake_waiters(Fid file, std::uint64_t seq) {
    auto it = attempt_waits.find(file);
    if(it == attempt_waits.end()) {
        return;
    }
    llvm::SmallVector<std::shared_ptr<async::event>, 2> woken;
    for(auto& wait: it->second) {
        if(wait.seq <= seq) {
            woken.push_back(std::move(wait.event));
        }
    }
    it->second.erase(
        ranges::remove_if(it->second, [&](const AttemptWait& wait) { return wait.seq <= seq; })
            .begin(),
        it->second.end());
    if(it->second.empty()) {
        attempt_waits.erase(it);
    }
    for(auto& event: woken) {
        event->set();
    }
}

async::task<> BackgroundIndex::run_pass() {
    auto& index = project.index;
    if(idle_timer) {
        co_await idle_timer->wait();
    }
    scheduled = false;

    if(reindexes.queued() == 0) {
        LOG_DEBUG("Background indexing: queue exhausted");
        // Nothing to index, yet the search index may be owed the full build
        // a previous session never reached — until it runs, every search
        // scans the symbol records — or a run of the entries it left
        // pending, or the start-up scan met a damaged record, which only a
        // save discards (a damaged database the save discarded queues
        // every unit, and the save starts their pass).
        if(index.damaged() || index.search_update_due(index::SearchRebuild::Idle)) {
            co_await project.save(index::SearchRebuild::Idle);
        }
        co_return;
    }

    active = true;
    LOG_DEBUG("Background indexing: starting, {} files queued", reindexes.queued());

    // FileVersion results hold for one pass: the disk can change under a
    // running pass, but staleness is re-judged per pass anyway.
    index.clear_staleness_cache();

    auto total =
        reindexes.begin_pass([&](Fid id) { return !project.dep_graph.module_of(id).empty(); });
    std::size_t dispatched = 0;
    PassState pass;

    // Announce the pass; a progress reporter reads the counts from
    // `progress` and owns the LSP token's begin/report/end handshake. With
    // no subscriber the signal is simply a no-op.
    current = {.stage = Progress::Stage::Begin, .total = total};
    on_progress_changed.emit();

    // Timed at the start of real work; the reporter's token handshake runs
    // off to the side and cannot inflate the reported indexing duration.
    ScopedTimer timer;
    // The dispatch loop runs as the first child of the group it fills, so a
    // shutdown cancel reaches the feeder and every in-flight task alike.
    co_await async::with_task_group(
        [&](async::task_group<>& workers) { return feed(workers, pass, total, dispatched); });

    current.stage = Progress::Stage::End;
    current.dispatched = dispatched;
    on_progress_changed.emit();

    // No dispatch loop takes from the queue any more.
    reindexes.end_pass();

    LOG_PERF("index",
             "phase=run dispatched={} skipped={} total={} elapsed_ms={}",
             dispatched,
             total - dispatched,
             total,
             timer.ms());
    co_await project.save(reindexes.queued() == 0 ? index::SearchRebuild::Idle
                                                  : index::SearchRebuild::Behind);
    // The pass's merge buffers are freed by now; glibc would keep their
    // pages, and the master its pass's peak, for the rest of the session.
    co_await release_pool_memory();

    // The pass owns the "active" gate through its save: releasing it
    // before the write await would let a next pass's save overlap this
    // one's in-flight batch, racing same-key blob writes on the pool.
    active = false;
    pass_ended.set();

    // Files enqueued or requeued while the pass ran saw their schedule
    // no-op against `active`; without this kick they would wait for the
    // next external event — and a content-changed pending file's index
    // stays skipped for that whole wait.
    if(reindexes.queued() > 0) {
        schedule(/*immediate=*/true);
    }
}

void BackgroundIndex::start(BackgroundOptions options) {
    if(state != State::Off) {
        return;
    }
    state = State::Started;
    this->options = options;
    auto& index = project.index;
    for(auto member: project.members()) {
        if(!index.indexed(member)) {
            enqueue(member, ReindexReason::DepsOnly);
        }
    }
    for(auto unit: index.outdated_units()) {
        enqueue(unit, ReindexReason::DepsOnly);
    }
    for(auto unit: index.owed_units()) {
        enqueue(unit, ReindexReason::ContentChanged);
    }
    schedule();
}

bool BackgroundIndex::enqueue(Fid file, ReindexReason reason, bool diagnostics) {
    // The one admission point of the background index: a file the build
    // does not compile as a unit, or a rule's `index = false` keeps out,
    // stays out here, whichever path asked — a cascade reaching a unit the
    // same change took out of the build included.
    if(!project.is_unit(file) || !project.rules.indexable(project.file_table.resolve(file))) {
        return false;
    }
    // A new pending reindex voids the running pass's freshness memos: one
    // taken after this recording must re-judge against the disk, or its
    // skip would finish the fresh sequence number on a result memoized
    // before the change and the file would never re-index.
    project.index.clear_staleness_cache();
    reindexes.record(file, reason, diagnostics);
    return true;
}

void BackgroundIndex::index_first(Fid file, bool diagnostics) {
    if(!enqueue(file, ReindexReason::DepsOnly, diagnostics)) {
        // No attempt will ever finish for an excluded file: a document
        // waiting on the index hears that now, or waits forever.
        on_attempt_finished.emit(file);
        return;
    }
    // The file someone is reading beats the bulk backlog.
    reindexes.move_to_front(file);
    schedule(/*immediate=*/true);
}

void BackgroundIndex::leave_to_document(Fid file) {
    documents_compile.insert(file);
}

void BackgroundIndex::take_back(Fid file) {
    documents_compile.erase(file);
    if(!project.file_table.disk.seen_missing(file) && !failed(file) &&
       enqueue(file, ReindexReason::DepsOnly)) {
        schedule();
    }
}

void BackgroundIndex::forget(Fid file) {
    reindexes.clear(file);
    wake_waiters(file);
}

async::task<> BackgroundIndex::await_attempt(Fid file) {
    auto pending = reindexes.peek(file);
    if(!pending) {
        co_return;
    }
    auto seq = pending->seq;
    auto& waits = attempt_waits[file];
    auto it = ranges::find(waits, seq, &AttemptWait::seq);
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

void BackgroundIndex::pause() {
    pause_depth += 1;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void BackgroundIndex::resume() {
    if(pause_depth > 0) {
        pause_depth -= 1;
    }
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

async::task<> BackgroundIndex::stop() {
    state = State::Stopped;
    tasks.cancel();
    co_await tasks.join();
    // Cancelling the pass that waited on the idle timer leaves it armed,
    // and an armed timer keeps the loop running until it fires.
    if(idle_timer) {
        idle_timer->stop();
    }
    // Cancelled tasks unwind before their finish bookkeeping; release any
    // parked feature request rather than stranding it past shutdown.
    for(auto& [file, waits]: attempt_waits) {
        for(auto& wait: waits) {
            wait.event->set();
        }
    }
    attempt_waits.clear();
}

void BackgroundIndex::schedule(bool immediate) {
    if(!started() || active) {
        return;
    }
    if(scheduled) {
        // An immediate request colliding with an armed idle timer re-arms
        // it to fire now — dropped, a reader asking for index_first would
        // wait out the full idle window an earlier ordinary schedule
        // started (s#9).
        if(immediate && idle_timer) {
            idle_timer->start(chrono::milliseconds(0));
        }
        return;
    }
    scheduled = true;

    if(!idle_timer) {
        idle_timer = std::make_shared<async::timer>(async::timer::create(*project.loop));
    }
    // Time is the feature: the idle timeout batches edit storms into one
    // pass. A follow-up pass for work requeued during the pass that just
    // ended has already been batched by that pass and starts right away —
    // a crashed file's retry must not owe an extra idle window on top of
    // the pass boundary it already waited out.
    idle_timer->start(
        chrono::milliseconds(immediate ? 0 : project.settings.project.idle_timeout_ms) +
        test_jitter());

    [[maybe_unused]] bool spawned = tasks.spawn(run_pass());
    assert(spawned && "the background index's task group refused a pass before it stopped");
}

async::task<> BackgroundIndex::wait_idle() {
    while(!idle()) {
        pass_ended.reset();
        co_await pass_ended.wait();
    }
}

}  // namespace clice
