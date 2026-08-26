#include "sched/index/pump.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <utility>

#include "sched/families/turun.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/pool.h"

#include "llvm/ADT/STLExtras.h"

namespace clice {

IndexPump::IndexPump(kota::event_loop& loop,
                     Workspace& workspace,
                     TURunFamily& turun,
                     IndexStore& store,
                     WorkerPool& pool) :
    loop(loop), bg_tasks(loop), workspace(workspace), turun(turun), store(store), pool(pool) {
    capacity_conn = pool.on_stateless_capacity.connect([this] { capacity_event.set(); });
}

void IndexPump::boost(std::uint32_t server_path_id) {
    enqueue(server_path_id, ReindexReason::DepsOnly);
    // Front of the un-consumed tail: the file someone is reading beats
    // the bulk backlog. A running round is not disturbed (its snapshot
    // semantics stay, see run_background_indexing); the slot then leads
    // the next round.
    auto begin = index_queue.begin() + index_queue_pos;
    auto it = std::find(begin, index_queue.end(), server_path_id);
    if(it != index_queue.end()) {
        std::rotate(begin, it, it + 1);
    }
    schedule(/*immediate=*/true);
}

void IndexPump::enqueue(std::uint32_t server_path_id, ReindexReason reason) {
    if(!ledger.record(server_path_id, reason)) {
        return;
    }
    index_queue.push_back(server_path_id);
}

void IndexPump::claim_report(const IndexStore::Report& report) {
    for(auto id: report.reindex()) {
        enqueue(id, ReindexReason::ContentChanged);
    }
    if(!report.rows_changed().empty()) {
        on_rows_changed.emit(report.rows_changed());
    }
}

llvm::SmallVector<std::uint32_t> IndexPump::save_debt() const {
    llvm::SmallVector<std::uint32_t> debt(failed_ids.begin(), failed_ids.end());
    auto pending = ledger.pending_files();
    debt.append(pending.begin(), pending.end());
    return debt;
}

kota::task<> IndexPump::await_attempt(std::uint32_t server_path_id) {
    auto pending = ledger.peek(server_path_id);
    if(!pending) {
        co_return;
    }
    auto ticket = pending->ticket;
    auto& waits = attempt_waits[server_path_id];
    auto it = llvm::find_if(waits, [&](const AttemptWait& wait) { return wait.ticket == ticket; });
    if(it == waits.end()) {
        waits.push_back({ticket, std::make_shared<kota::event>()});
        it = waits.end() - 1;
    }
    // Hold the shared_ptr across the await: the settle moves the event out
    // of the map before setting it, and a fresh wait after that parks on a
    // new instance.
    auto event = it->event;
    co_await event->wait();
}

void IndexPump::settle_attempt_waits(std::uint32_t server_path_id, std::uint64_t ticket) {
    auto it = attempt_waits.find(server_path_id);
    if(it == attempt_waits.end()) {
        return;
    }
    llvm::SmallVector<std::shared_ptr<kota::event>, 2> settled;
    for(auto& wait: it->second) {
        if(wait.ticket <= ticket) {
            settled.push_back(std::move(wait.event));
        }
    }
    llvm::erase_if(it->second, [&](const AttemptWait& wait) { return wait.ticket <= ticket; });
    if(it->second.empty()) {
        attempt_waits.erase(it);
    }
    for(auto& event: settled) {
        event->set();
    }
}

void IndexPump::pause_indexing() {
    pause_depth += 1;
    if(pause_depth == 1) {
        resume_event.reset();
        LOG_DEBUG("Background indexing paused");
    }
}

void IndexPump::resume_indexing() {
    if(pause_depth > 0)
        pause_depth -= 1;
    if(pause_depth == 0) {
        resume_event.set();
        LOG_DEBUG("Background indexing resumed");
    }
}

kota::task<> IndexPump::stop() {
    bg_tasks.cancel();
    co_await bg_tasks.join();
    // Cancelled tasks unwind before their settle bookkeeping; release any
    // parked feature request rather than stranding it past shutdown.
    for(auto& waits: llvm::make_second_range(attempt_waits)) {
        for(auto& wait: waits) {
            wait.event->set();
        }
    }
    attempt_waits.clear();
}

void IndexPump::schedule(bool immediate) {
    if(!workspace.config.project.enable_indexing.value || indexing_active)
        return;
    if(indexing_scheduled) {
        // An immediate request colliding with an armed idle timer re-arms
        // it to fire now — dropped, a boosted reader would wait out the
        // full idle window an earlier ordinary schedule started (s#9).
        if(immediate && index_idle_timer) {
            index_idle_timer->start(std::chrono::milliseconds(0));
        }
        return;
    }
    indexing_scheduled = true;

    if(!index_idle_timer) {
        index_idle_timer = std::make_shared<kota::timer>(kota::timer::create(loop));
    }
    // The idle timeout exists to batch edit storms into one round; a
    // follow-up round for work requeued during the round that just ended
    // has already been batched by that round and starts right away — a
    // crashed file's retry must not owe an extra idle window on top of the
    // round boundary it already waited out.
    index_idle_timer->start(
        std::chrono::milliseconds(immediate ? 0 : workspace.config.project.idle_timeout_ms.value));

    if(!bg_tasks.spawn(run_background_indexing())) {
        indexing_scheduled = false;
        LOG_WARN("Failed to spawn background indexing task (task group stopped)");
    }
}

auto IndexPump::note_dispatch_failure(const PendingLedger::Claim& claim, bool crashed)
    -> PendingLedger::FailureVerdict {
    auto outcome = ledger.on_dispatch_failure(claim, crashed);
    if(outcome.needs_slot) {
        index_queue.push_back(claim.id);
    }
    // Giving up cleared the ledger; no further attempt will come, so any
    // parked waiter must not be left waiting for one.
    if(outcome.verdict == PendingLedger::FailureVerdict::GaveUp) {
        settle_attempt_waits(claim.id);
    }
    return outcome.verdict;
}

kota::task<> IndexPump::run_round_feeder(kota::task_group<>& workers,
                                         RoundState& round,
                                         std::size_t round_end,
                                         std::size_t total,
                                         std::size_t& dispatched) {
    while(index_queue_pos < round_end) {
        // Every wait loops back to re-check ALL gates: a pause arriving
        // while parked on capacity (or a capacity loss while parked on the
        // pause) must not let one slot slip through on wake-up.
        while(true) {
            if(pause_depth > 0) {
                co_await resume_event.wait();
                continue;
            }

            // With no schedulable worker but revival pending, a dispatch
            // would only convert the queue slot into an instant
            // worker_unavailable failure; park until a slot returns to
            // service. With revival off such failures are terminal and
            // take the normal failure path.
            if(pool.revives_slots() && pool.schedulable_stateless() == 0) {
                capacity_event.reset();
                co_await capacity_event.wait();
                continue;
            }

            // Feed at most twice the pool's low budget: deep enough that
            // workers never idle waiting for the feeder, shallow enough
            // that the pool queue holds little when a pause or budget cut
            // lands. The floor keeps the window alive when the budget
            // reads zero (a pool that has not started yet); without it
            // the feeder would wait on task_done with nothing in flight
            // to ever set it.
            if(round.inflight >= std::max<std::size_t>(2 * pool.effective_low_limit(), 2)) {
                round.task_done.reset();
                co_await round.task_done.wait();
                continue;
            }
            break;
        }

        auto server_path_id = index_queue[index_queue_pos];
        index_queue_pos += 1;
        // No open-session or hash-freshness shortcut here: the index task
        // is the single decision point for skipping (it knows the pending
        // reason; a hash check alone cannot see a file's own edit), and
        // its settle retires the claimed debt with newer recordings
        // honored. A second, reason-blind copy of these checks here is
        // exactly what once erased ContentChanged state early and let a
        // stale shard keep serving.

        // A queued slot whose debt was cleared mid-batch (the file was
        // removed from disk) has nothing to index — skip the slot. Every
        // other slot has an entry, because enqueue books it before the
        // queue push.
        auto claim = ledger.claim(server_path_id);
        if(!claim) {
            continue;
        }

        dispatched += 1;
        round.inflight += 1;
        // A member coroutine, not an immediately-invoked capturing lambda:
        // a lambda's captures live in the lambda object, which dies at the
        // end of this statement — anything read after the first suspension
        // would dangle. Coroutine parameters are copied into the frame.
        workers.spawn(run_index_task(*claim, dispatched, total, round));
    }

    LOG_DEBUG("Background indexing: all {} tasks spawned, waiting for completion", dispatched);
}

kota::task<> IndexPump::run_index_task(PendingLedger::Claim claim,
                                       std::size_t index,
                                       std::size_t total,
                                       RoundState& round) {
    auto server_path_id = claim.id;
    // Dispatch-time admission: the serving side may veto the work. A veto
    // settles the claimed debt below (an ordinary open session's skip must
    // clear it, or the pump spins); only a Defer keeps the debt for a
    // later round.
    auto admit = admission ? admission(server_path_id) : Admission::Admit;
    if(admit == Admission::Admit) {
        auto file_path = std::string(workspace.path_pool.resolve(server_path_id));
        // The engine's own observation is authoritative for content
        // changes: it saw the event. The dep-hash check cannot be trusted
        // to see a file's own edit (it validates the recorded
        // dependencies), so only deps-only slots — where it exists to
        // deduplicate cascade storms — may take the shortcut.
        if(ledger.pending_reason(server_path_id) == ReindexReason::ContentChanged ||
           store.need_update(file_path)) {
            LOG_INFO("[{}/{}] Indexing {}", index, total, file_path);
            auto outcome = co_await turun.run(
                server_path_id,
                {.index = true},
                {.superseded = [this, claim] { return ledger.superseded(claim); },
                 .landing =
                     [this, server_path_id] {
                         return admission ? admission(server_path_id) : Admission::Admit;
                     }});
            if(outcome.verdict == TURunFamily::Verdict::Shutdown) {
                // The graph refused the round: nothing ran, so the debt
                // stays booked for the final snapshot and the next
                // session; only the round bookkeeping below still runs so
                // a live feeder is not left waiting on this slot.
                round.completed += 1;
                round.inflight -= 1;
                round.task_done.set();
                co_return;
            }
            // The report's debt is claimed before the settle and the
            // waiter wake-ups below: a waker re-deriving its route must
            // never observe rows as settled that the merge just declared
            // stale.
            claim_report(outcome.report);
            admit = outcome.landing;
            switch(outcome.verdict) {
                case TURunFamily::Verdict::Completed: {
                    failed_ids.erase(server_path_id);
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
                case TURunFamily::Verdict::Skipped: {
                    break;
                }
                case TURunFamily::Verdict::Failed: {
                    LOG_WARN("[{}/{}] Index failed for {}: {}",
                             index,
                             total,
                             file_path,
                             outcome.error);
                    failed_ids.insert(server_path_id);
                    break;
                }
                case TURunFamily::Verdict::Crashed:
                case TURunFamily::Verdict::Preempted: {
                    // Preempted under memory pressure or lost to a worker
                    // crash: the work itself is fine — requeue the file
                    // with its original reason so the next round redoes it
                    // instead of silently dropping coverage. Only crashes
                    // spend the bounded budget.
                    bool crashed = outcome.verdict == TURunFamily::Verdict::Crashed;
                    switch(note_dispatch_failure(claim, crashed)) {
                        case PendingLedger::FailureVerdict::Dropped: {
                            LOG_INFO("[{}/{}] Index dropped for removed file {}",
                                     index,
                                     total,
                                     file_path);
                            break;
                        }
                        case PendingLedger::FailureVerdict::Superseded: {
                            LOG_INFO("[{}/{}] Index failure for superseded content of {}",
                                     index,
                                     total,
                                     file_path);
                            break;
                        }
                        case PendingLedger::FailureVerdict::GaveUp: {
                            // Log-only by design: the file is usually not
                            // open (open documents are served by their
                            // session, not the shard), so there is no
                            // diagnostic surface. Cross-file references
                            // into this file stay stale until its content
                            // changes.
                            LOG_WARN(
                                "[{}/{}] Index giving up on {} after {} crash requeues; "
                                "its cross-file data stays stale until it is edited: {}",
                                index,
                                total,
                                file_path,
                                max_requeue_attempts,
                                outcome.error);
                            failed_ids.insert(server_path_id);
                            break;
                        }
                        case PendingLedger::FailureVerdict::Requeued: {
                            LOG_INFO("[{}/{}] Index requeued for {}: {}",
                                     index,
                                     total,
                                     file_path,
                                     outcome.error);
                            break;
                        }
                    }
                    break;
                }
                case TURunFamily::Verdict::Shutdown: {
                    std::unreachable();
                }
            }
        }
    }
    // The pending window ends with the index attempt, success or not. On
    // failure the last-known rows resume serving — deliberately: keeping
    // the gate would hide a file that fails to index (broken compile,
    // missing command) from every cross-file query with no recovery path,
    // since only a future event re-enqueues it. Any such event re-judges
    // staleness by content hash. A re-enqueue during the flight booked
    // newer debt: the settle leaves it standing.
    if(admit != Admission::Defer) {
        ledger.settle(claim);
    }
    // The attempt settled with no retry pending; the serving side judges
    // whether an open session still waiting on the index can ever be
    // served by it, before its waiters wake (contract 15).
    if(on_attempt_settled && !ledger.contains(server_path_id)) {
        on_attempt_settled(server_path_id);
    }
    // Wake the waiters parked on this attempt (and older tickets it
    // covers) after the escalation above, so they re-derive their route
    // against the settled state. Waiters of a requeue made during the
    // flight bound the fresh ticket and stay parked for that newer
    // attempt.
    settle_attempt_waits(server_path_id, claim.ticket);
    round.completed += 1;
    round.inflight -= 1;
    round.task_done.set();
    progress_data.stage = Progress::Stage::Report;
    progress_data.completed = round.completed;
    on_progress_changed.emit();
}

kota::task<> IndexPump::run_background_indexing() {
    if(index_idle_timer) {
        co_await index_idle_timer->wait();
    }
    indexing_scheduled = false;

    if(index_queue_pos >= index_queue.size()) {
        LOG_DEBUG("Background indexing: queue exhausted");
        co_return;
    }

    indexing_active = true;
    LOG_DEBUG("Background indexing: starting, {} files queued",
              index_queue.size() - index_queue_pos);

    // FileVersion verdicts hold for one round: the disk can change under a
    // running round, but staleness is re-judged per round anyway.
    store.begin_round();

    std::stable_partition(
        index_queue.begin() + index_queue_pos,
        index_queue.end(),
        [this](std::uint32_t id) { return workspace.path_to_module.contains(id); });

    // This round consumes [index_queue_pos, round_end) only. Anything
    // appended during the round — including its own failures' requeues —
    // waits for the next round; consuming a requeue in the round that
    // produced it is what let a worker outage spin the dispatch loop
    // against instant failures (#611).
    auto round_end = index_queue.size();
    auto total = round_end - index_queue_pos;
    std::size_t dispatched = 0;
    RoundState round;

    // Announce the round; a progress reporter reads the counts from
    // progress() and owns the LSP token's begin/report/end handshake. With
    // no subscriber the signal is simply a no-op.
    progress_data = Progress{.stage = Progress::Stage::Begin, .total = total};
    on_progress_changed.emit();

    // Timed at the start of real work; the reporter's token handshake runs
    // off to the side and cannot inflate the reported indexing duration.
    ScopedTimer timer;
    kota::task_group<> workers(loop);

    // The dispatch loop runs as a child of `workers`, so this frame's only
    // suspension while children live is the join below: a shutdown cancel
    // cascades through the join into the group, and the feeder plus every
    // in-flight task unwind before `workers` is destroyed. Parking the
    // feeder's waits on this frame instead would let the cancel finalize
    // the frame — destroying the group with children still in flight.
    workers.spawn(run_round_feeder(workers, round, round_end, total, dispatched));
    co_await workers.join();

    // Skipped files bump `completed` without a Report emit; refresh the
    // materialized count so a subscriber waking up on End reads the truth.
    progress_data.completed = round.completed;
    progress_data.stage = Progress::Stage::End;
    progress_data.dispatched = dispatched;
    on_progress_changed.emit();

    // Safe point to compact: no dispatch loop holds an index into the queue.
    // Files enqueued or requeued past the round snapshot keep the queue
    // alive for the next scheduled round.
    if(index_queue_pos >= index_queue.size()) {
        assert(!ledger.has_queued_slots() && "a drained queue must leave no unconsumed slot");
        index_queue.clear();
        index_queue_pos = 0;
    }

    LOG_PERF("index",
             "phase=run dispatched={} skipped={} total={} elapsed_ms={}",
             dispatched,
             total - dispatched,
             total,
             timer.ms());
    claim_report(co_await store.save(save_debt()));

    // The round owns the "active" gate through its save: releasing it
    // before the write await would let a next round's save overlap this
    // one's in-flight batch, racing same-key blob writes on the pool.
    indexing_active = false;

    // Files enqueued or requeued while the round ran saw their schedule()
    // no-op against indexing_active; without this kick they would wait for
    // the next external event — and a content-changed pending file's rows
    // stay skipped for that whole wait.
    if(index_queue_pos < index_queue.size()) {
        schedule(/*immediate=*/true);
    }
}

}  // namespace clice
