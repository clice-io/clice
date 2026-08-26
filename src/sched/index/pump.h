#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "sched/index/ledger.h"
#include "sched/index/store.h"
#include "sched/workspace.h"
#include "support/signal.h"

#include "kota/async/async.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

class TURunFamily;
class WorkerPool;

namespace testing {

struct IndexerFixture;

}

/// The background indexing pump: the standing-interest scheduler that
/// drives disk files through TURun rounds. It owns the queue, the debt
/// ledger with its claim/settle contract and requeue budget, the idle
/// batching timer and the feeder window — and nothing about what a run
/// produces (the TURun family) or where results live (the IndexStore).
///
/// The pump is serving-neutral: it never sees a SessionStore. The serving
/// side injects its vetoes through `admission` and its escalation through
/// `on_attempt_settled`; a batch driver installs neither and everything is
/// admitted.
class IndexPump {
public:
    IndexPump(kota::event_loop& loop,
              Workspace& workspace,
              TURunFamily& turun,
              IndexStore& store,
              WorkerPool& pool);

    /// Dispatch- and landing-time admission on one claimed file, supplied
    /// by the serving side (open sessions veto); null admits everything.
    /// A veto settles the claimed debt — an ordinary open session's skip
    /// must clear it, or the pump spins; only Defer keeps the debt for a
    /// later round.
    std::function<Admission(std::uint32_t)> admission;

    /// Invoked when an index attempt settled with no retry pending, before
    /// the attempt's waiters wake (contract 15): the serving side decides
    /// whether the settled state can ever serve an open session and
    /// escalates it otherwise, so the waking waiters re-derive their route
    /// against the escalated state.
    std::function<void(std::uint32_t path_id)> on_attempt_settled;

    /// Emitted when store rows that may be index-served changed (merged,
    /// re-masked, dropped or shed). Carries the affected path_ids; the
    /// serving adapter checks which of them an open session serves and
    /// refreshes those clients.
    Signal<llvm::ArrayRef<std::uint32_t>> on_rows_changed;

    /// Temporarily pause background indexing to give priority to user
    /// requests.  Indexing tasks already dispatched to workers continue,
    /// but no new tasks will be sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// RAII guard that pauses indexing for its lifetime.
    struct [[nodiscard]] ScopedPause {
        IndexPump& pump;

        explicit ScopedPause(IndexPump& pump) : pump(pump) {
            pump.pause_indexing();
        }

        ~ScopedPause() {
            pump.resume_indexing();
        }

        ScopedPause(const ScopedPause&) = delete;
        ScopedPause& operator=(const ScopedPause&) = delete;
    };

    ScopedPause scoped_pause() {
        return ScopedPause{*this};
    }

    /// Add a file to the background indexing queue. A file enqueued twice
    /// keeps a single queue entry; its reason is upgraded to ContentChanged
    /// if either enqueue says so (a file both cascaded onto and edited is
    /// as stale as the edit makes it).
    void enqueue(std::uint32_t server_path_id, ReindexReason reason);

    /// Someone is reading `server_path_id` through the index and nothing
    /// serves it yet: enqueue it at the front of the un-consumed queue and
    /// start a round without the idle delay. A running round finishes
    /// undisturbed; the file then leads the next one.
    void boost(std::uint32_t server_path_id);

    /// Wait until the pending (re)index attempt observed at call time
    /// settles — the merge landed, the attempt gave up, or the entry was
    /// cleared — and return immediately when nothing is pending. One
    /// attempt, not a settled file: a requeue landing during the flight
    /// does not extend the wait. For replies clients cache with no
    /// refresh request (outline, links): answered while the didOpen
    /// boost is still running, the empty result would freeze until the
    /// next edit.
    kota::task<> await_attempt(std::uint32_t server_path_id);

    /// Why the file awaits re-indexing (queued or currently being indexed),
    /// or nullopt when its index is not pending an update. O(1), no I/O —
    /// the query path calls this per candidate file.
    std::optional<ReindexReason> pending_reason(std::uint32_t server_path_id) const {
        return ledger.pending_reason(server_path_id);
    }

    /// Forget a file's pending-reindex state (reason and queue membership):
    /// used when the file is removed from disk — nothing is left to reindex,
    /// and a lingering ContentChanged reason would suppress its deliberately
    /// still-serving shard forever. A queue slot already consumed stays
    /// consumed; one not yet consumed is skipped at dispatch time (the
    /// consume loop treats a missing ledger entry as a cleared slot).
    void clear_pending(std::uint32_t server_path_id) {
        ledger.clear(server_path_id);
        settle_attempt_waits(server_path_id);
    }

    /// Schedule background indexing (respects idle timeout and dedup).
    /// `immediate` skips the idle batching window — used by the round tail
    /// for work requeued during the round that just ended, and by boost,
    /// which re-arms an already-armed timer to fire now (waiting out a
    /// previously scheduled idle window would starve the reader).
    void schedule(bool immediate = false);

    /// Claim a store report: book its reindex debt into the ledger — before
    /// the current attempt settles and its waiters wake, so a waker never
    /// observes rows as settled that the report just declared stale — and
    /// surface its serving-row changes through on_rows_changed.
    void claim_report(const IndexStore::Report& report);

    /// The pump debt a save()'s CDB snapshot persists: files whose latest
    /// attempt failed for good plus everything still booked in the ledger.
    llvm::SmallVector<std::uint32_t> save_debt() const;

    /// Cancel background indexing and wait for all tasks to settle.
    kota::task<> stop();

    /// Whether background indexing is currently idle (no active or queued work).
    bool is_idle() const {
        return !indexing_active && index_queue_pos >= index_queue.size();
    }

    /// Number of files remaining in the indexing queue.
    std::size_t pending_files() const {
        return index_queue_pos < index_queue.size() ? index_queue.size() - index_queue_pos : 0;
    }

    /// Total files that were enqueued in the current (or last) indexing round.
    std::size_t total_queued() const {
        return index_queue.size();
    }

    /// Files whose latest index attempt failed for good — rejected by the
    /// worker, an empty or unverifiable result, a spent crash budget, or a
    /// dead IPC path — with no retry pending. Their rows are missing or
    /// stale; a later successful pass removes them again. The one-shot
    /// `clice index` reports a partial build from this.
    std::size_t failed_files() const {
        return failed_ids.size();
    }

    /// Progress of the current (or last) indexing round. The reporter reads
    /// this on each on_progress_changed emission — the signal only wakes it,
    /// the numbers live here.
    struct Progress {
        enum class Stage : std::uint8_t { Begin, Report, End };
        Stage stage = Stage::Begin;
        std::size_t total = 0;
        std::size_t completed = 0;
        std::size_t dispatched = 0;
    };

    const Progress& progress() const {
        return progress_data;
    }

    /// Emitted whenever the indexing progress state changes (round begins, a
    /// file completes, round ends). A subscriber reads progress() on wake.
    Signal<> on_progress_changed;

private:
    friend struct testing::IndexerFixture;

    kota::event_loop& loop;
    kota::task_group<> bg_tasks;
    Workspace& workspace;
    TURunFamily& turun;
    IndexStore& store;
    WorkerPool& pool;

    /// Background indexing queue and scheduling state. The ledger tracks
    /// which files hold an un-consumed slot so enqueue can dedupe; the
    /// queue is compacted once a round has fully drained.
    std::vector<std::uint32_t> index_queue;
    std::size_t index_queue_pos = 0;

    /// The pending-reindex debt: claim/settle bookkeeping, tickets and
    /// the crash-requeue budget live in the ledger. The pump-side rules
    /// on top of it, each born from a concrete bug:
    /// 1. The admission + freshness checks inside the index task are the
    ///    ONLY places that decide to skip work. Duplicating them at the
    ///    feeder reintroduces reason-blind skips.
    /// 2. need_update() may shortcut deps-only slots ONLY: the engine
    ///    observed content changes itself, and the dep-hash check cannot
    ///    see a file's own edit.
    PendingLedger ledger{max_requeue_attempts};

    constexpr static unsigned max_requeue_attempts = 3;

    /// Merge a failed dispatch's claim back through the ledger and apply
    /// the pump-side halves: a Requeued file gets its queue slot, an
    /// abandoned one wakes its waiters.
    PendingLedger::FailureVerdict note_dispatch_failure(const PendingLedger::Claim& claim,
                                                        bool crashed);

    /// Wake the file's await_attempt waiters whose observed ticket the
    /// settled attempt covers (`ticket` and older) and drop their events.
    /// The default wakes every waiter — for the paths where no further
    /// attempt will come (entry cleared, shutdown).
    void settle_attempt_waits(std::uint32_t server_path_id, std::uint64_t ticket = -1);

    llvm::DenseSet<std::uint32_t> failed_ids;

    struct AttemptWait {
        std::uint64_t ticket;
        std::shared_ptr<kota::event> event;
    };

    /// The events await_attempt waiters park on, per file and per pending
    /// ticket observed at wait time; each fires when an attempt covering
    /// its ticket settles (or wholesale at stop()). Keyed by ticket so a
    /// requeue during an attempt's flight cannot extend earlier waits —
    /// await_attempt promises one attempt, not a settled file.
    llvm::DenseMap<std::uint32_t, llvm::SmallVector<AttemptWait, 2>> attempt_waits;
    bool indexing_active = false;
    bool indexing_scheduled = false;
    std::shared_ptr<kota::timer> index_idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event.
    /// Uses a counter so nested pause/resume pairs work correctly.
    std::size_t pause_depth = 0;
    kota::event resume_event{true};

    /// Set by on_stateless_capacity: wakes a round parked on "no schedulable
    /// stateless worker" the moment a slot (re)enters service.
    kota::event capacity_event{false};
    Signal<>::Connection capacity_conn;

    Progress progress_data;

    /// A round's shared counters, living on run_background_indexing's frame,
    /// which outlives every spawned task (it joins them before returning).
    struct RoundState {
        std::size_t completed = 0;

        /// Dispatched tasks not yet finished.
        std::size_t inflight = 0;

        /// Set whenever a task finishes, waking a feeder waiting out the cap.
        kota::event task_done{false};
    };

    kota::task<> run_background_indexing();

    /// The round's dispatch loop, spawned as a child of `workers` so that a
    /// shutdown cancel reaches it through the round frame's join — see the
    /// spawn site. Consumes [index_queue_pos, round_end) and spawns one
    /// run_index_task per live slot, bounded by the feeder window.
    kota::task<> run_round_feeder(kota::task_group<>& workers,
                                  RoundState& round,
                                  std::size_t round_end,
                                  std::size_t total,
                                  std::size_t& dispatched);

    /// One dispatched unit of a background round: admit, run the TU through
    /// its TURun node, claim the merge report, settle the claimed debt and
    /// report progress.
    kota::task<> run_index_task(PendingLedger::Claim claim,
                                std::size_t index,
                                std::size_t total,
                                RoundState& round);
};

}  // namespace clice
