module;

#include "modules/prelude.h"

module clice:sched.index.pump;

import :project.index_store;
import :project.project;
import :sched.families.turun;
import :sched.graph;
import :sched.index.pending_table;
import :support.signal;
import :worker.pool;

namespace clice {

namespace testing {

struct IndexerFixture;

}

/// The background indexing pump: the standing-interest scheduler that
/// drives disk files through TURun rounds. It owns the queue, the debt
/// table with its take/finish contract and requeue budget, the idle
/// batching timer and the feeder window — and nothing about what a run
/// produces (the TURun family) or where results live (the IndexStore).
///
/// The pump is serving-neutral: it never sees a SessionStore. The serving
/// side injects its policy through `compiled_by_session` and
/// `on_attempt_finished`; a batch driver installs neither.
class IndexPump {
public:
    IndexPump(kota::event_loop& loop,
              Project& project,
              TURunFamily& turun,
              IndexStore& store,
              WorkerPool& pool);

    /// Invoked when an index attempt finished with no retry pending, before
    /// the attempt's waiters wake (contract 15): the serving side decides
    /// whether the finished state can ever serve an open session and
    /// escalates it otherwise, so the waking waiters re-derive their route
    /// against the escalated state. Also invoked for a boost the pump
    /// refuses — a file a rule keeps out of the index — since no attempt
    /// will ever finish for it.
    std::function<void(Fid path_id)> on_attempt_finished;

    /// Whether an open document's own compile serves the file: debt other
    /// than a change of its own content then is cleared without a background
    /// compile, which would compile the file a second time. The serving
    /// side enqueues the file again when the document closes.
    std::function<bool(Fid path_id)> compiled_by_session;

    /// Emitted when store shards that may be index-served changed (merged,
    /// re-masked, dropped or shed). Carries the affected path_ids; the
    /// serving adapter checks which of them an open session serves and
    /// refreshes those clients.
    Signal<llvm::ArrayRef<Fid>> on_index_changed;

    /// Temporarily pause background indexing to give priority to user
    /// requests.  Indexing tasks already dispatched to workers continue,
    /// but no new tasks will be sent until resume_indexing() is called.
    void pause_indexing();

    /// Resume background indexing after a pause.
    void resume_indexing();

    /// Pause indexing once `units` more files are dispatched, so exactly
    /// that many finish before the pause holds (CLICE_TEST_PAUSE).
    void pause_after(std::size_t units);

    /// RAII guard that pauses indexing for its lifetime; a moved-from one
    /// holds nothing.
    struct [[nodiscard]] ScopedPause {
        IndexPump* pump;

        explicit ScopedPause(IndexPump& pump) : pump(&pump) {
            pump.pause_indexing();
        }

        ScopedPause(ScopedPause&& other) noexcept : pump(std::exchange(other.pump, nullptr)) {}

        ScopedPause& operator=(ScopedPause&&) = delete;

        ~ScopedPause() {
            if(pump) {
                pump->resume_indexing();
            }
        }
    };

    ScopedPause scoped_pause() {
        return ScopedPause{*this};
    }

    /// Add a file to the background indexing queue. A file enqueued twice
    /// keeps a single queue entry; its reason is upgraded to ContentChanged
    /// if either enqueue says so (a file both cascaded onto and edited is
    /// as stale as the edit makes it). False when a rule keeps the file out
    /// of the index: nothing is queued and no attempt will ever finish.
    bool enqueue(Fid server_path_id, ReindexReason reason);

    /// Someone is reading `server_path_id` through the index and nothing
    /// serves it yet: enqueue it at the front of the un-consumed queue and
    /// start a round without the idle delay. A running round finishes
    /// undisturbed; the file then leads the next one.
    void boost(Fid server_path_id);

    /// Wait until the pending (re)index attempt observed at call time
    /// finishes — the merge committed, the attempt gave up, or the entry was
    /// cleared — and return immediately when nothing is pending. One
    /// attempt, not an idle file: a requeue arriving during the flight
    /// does not extend the wait. For replies clients cache with no
    /// refresh request (outline, links): answered while the didOpen
    /// boost is still running, the empty result would freeze until the
    /// next edit.
    kota::task<> await_attempt(Fid server_path_id);

    /// Why the file awaits re-indexing (queued or currently being indexed),
    /// or nullopt when its index is not pending an update. O(1), no I/O —
    /// the query path calls this per candidate file.
    std::optional<ReindexReason> pending_reason(Fid server_path_id) const {
        return pending_table.pending_reason(server_path_id);
    }

    /// Forget a file's pending-reindex debt: used when the file is removed
    /// from disk — nothing is left to reindex, and a lingering
    /// ContentChanged reason would suppress its deliberately still-serving
    /// shard forever. A queue slot already consumed stays consumed; one not
    /// yet consumed is skipped at dispatch time (the consume loop treats a
    /// missing table entry as a cleared slot) unless the file is recorded
    /// again first, which reuses it.
    void clear_pending(Fid server_path_id) {
        pending_table.clear(server_path_id);
        wake_attempt_waits(server_path_id);
    }

    /// Schedule background indexing (respects idle timeout and dedup).
    /// `immediate` skips the idle batching window — used by the round tail
    /// for work requeued during the round that just ended, and by boost,
    /// which re-arms an already-armed timer to fire now (waiting out a
    /// previously scheduled idle window would starve the reader).
    void schedule(bool immediate = false);

    /// Take a store report: book its reindex debt into the table — before
    /// the current attempt finishes and its waiters wake, so a waker never
    /// observes an index as current that the report just declared stale —
    /// and report its serving-index changes through on_index_changed.
    void apply_report(const IndexStore::Report& report);

    /// The pump debt a save()'s CDB snapshot persists: files whose latest
    /// attempt failed for good plus everything still booked in the table.
    llvm::SmallVector<Fid> save_debt() const;

    /// Save the index with this pump's debt and take the report, again
    /// while a save owes a retry (a grown map can still be too small).
    kota::task<> persist(IndexStore::SearchRebuild search = IndexStore::SearchRebuild::Behind);

    /// Cancel background indexing and wait for all tasks to finish.
    kota::task<> stop();

    /// Whether background indexing is currently idle (no active or queued work).
    bool is_idle() const {
        return !indexing_active && index_queue_pos >= index_queue.size();
    }

    /// Wait until is_idle(): every round, requeue rounds included, has run
    /// and saved its results.
    kota::task<> wait_idle();

    /// Number of files remaining in the indexing queue.
    std::size_t pending_files() const {
        return index_queue_pos < index_queue.size() ? index_queue.size() - index_queue_pos : 0;
    }

    /// Files whose latest index attempt failed for good — rejected by the
    /// worker, an empty or unverifiable result, a spent crash budget, or a
    /// dead IPC path — with no retry pending. Their index is missing or
    /// stale; a later successful pass removes them again. The one-shot
    /// `clice index` reports a partial build from this.
    const llvm::DenseSet<Fid>& failed() const {
        return failed_ids;
    }

    /// TUs actually indexed and merged this session — dispatches the
    /// freshness gate skipped do not count. The one-shot `clice index`
    /// reports its work from this, so a warm rerun honestly says 0.
    std::size_t indexed_files() const {
        return indexed_total;
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
    Project& project;
    TURunFamily& turun;
    IndexStore& store;
    WorkerPool& pool;

    /// Background indexing queue and scheduling state. The pending table tracks
    /// which files hold an un-consumed slot so enqueue can dedupe; the
    /// queue is compacted once a round has fully drained.
    std::vector<Fid> index_queue;
    std::size_t index_queue_pos = 0;
    /// Where the running round's slots end. A slot past it waits for the
    /// next round and may be a file the running round has in flight.
    std::size_t round_end = 0;

    /// The pending-reindex debt: take/finish bookkeeping, tickets and
    /// the crash-requeue budget live in the pending table. The pump-side rules
    /// on top of it, each born from a concrete bug:
    /// 1. The freshness check inside the index task is the ONLY place
    ///    that decides to skip work. Duplicating it at the feeder
    ///    reintroduces reason-blind skips.
    /// 2. need_update() may shortcut deps-only slots ONLY: the engine
    ///    observed content changes itself, and the dep-hash check cannot
    ///    see a file's own edit.
    PendingTable pending_table{max_requeue_attempts};

    constexpr static unsigned max_requeue_attempts = 3;

    /// Merge a failed dispatch's taken debt back through the table and apply
    /// the pump-side halves: a Requeued file gets its queue slot, an
    /// abandoned one wakes its waiters.
    PendingTable::FailureDecision note_dispatch_failure(const PendingTable::TakenDebt& taken,
                                                        PendingTable::Failure failure);

    /// Wake the file's await_attempt waiters whose observed ticket the
    /// finished attempt covers (`ticket` and older) and drop their events.
    /// The default wakes every waiter — for the paths where no further
    /// attempt will come (entry cleared, shutdown).
    void wake_attempt_waits(Fid server_path_id, std::uint64_t ticket = -1);

    llvm::DenseSet<Fid> failed_ids;
    std::size_t indexed_total = 0;

    struct AttemptWait {
        std::uint64_t ticket;
        std::shared_ptr<kota::event> event;
    };

    /// The events await_attempt waiters park on, per file and per pending
    /// ticket observed at wait time; each fires when an attempt covering
    /// its ticket finishes (or wholesale at stop()). Keyed by ticket so a
    /// requeue during an attempt's flight cannot extend earlier waits —
    /// await_attempt promises one attempt, not an idle file.
    llvm::DenseMap<Fid, llvm::SmallVector<AttemptWait, 2>> attempt_waits;
    bool indexing_active = false;
    bool indexing_scheduled = false;

    /// Set whenever a round ends; wait_idle() waits on it.
    kota::event round_ended{false};
    std::shared_ptr<kota::timer> index_idle_timer;

    /// Pause/resume: when paused, new index tasks wait on this event.
    /// Uses a counter so nested pause/resume pairs work correctly.
    std::size_t pause_depth = 0;
    kota::event resume_event{true};

    /// Dispatches left before pause_after() pauses; zero when none is due.
    std::size_t dispatches_before_pause = 0;

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
                                  std::size_t total,
                                  std::size_t& dispatched);

    /// One dispatched unit of a background round: admit, run the TU through
    /// its TURun node, take the merge report, finish the taken debt and
    /// report progress.
    kota::task<> run_index_task(PendingTable::TakenDebt taken,
                                std::size_t index,
                                std::size_t total,
                                RoundState& round);
};

}  // namespace clice
