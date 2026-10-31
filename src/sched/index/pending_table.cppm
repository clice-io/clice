module clice:sched.index.pending_table;

import :support.prelude;
import :vfs.file_table;

namespace clice {

namespace testing {

struct IndexerFixture;

}

/// Why a file awaits re-indexing. The invalidation engine knows the cause
/// at enqueue time, so queries can decide in O(1) whether a pending file's
/// existing index is still trustworthy (see IndexQuery's freshness
/// contract).
enum class ReindexReason : std::uint8_t {
    /// Enqueued by a dependency cascade (or a bulk pass of unknown
    /// staleness): the file's own content is not known to have changed, so
    /// its index is positionally intact — at worst semantically
    /// behind — and keeps serving until the reindex finishes.
    DepsOnly,
    /// The file's own content changed: its index describes text that
    /// no longer exists, so queries skip this file's contribution until
    /// the reindex finishes.
    ContentChanged,
};

/// The background index's table of pending reindexes, per file — the
/// take/finish contract behind background reindexing:
///
/// - record() books a pending reindex; within one queued slot
///   ContentChanged absorbs, while a fresh slot after consumption carries
///   its own kind (the consumed pass owns the earlier one).
/// - take() atomically takes the file's pending reindex at dispatch;
///   events arriving during the flight book into the entry as a newer one.
/// - finish() consumes exactly the taken reindex: a newer recording
///   survives an older attempt's completion.
/// - on_dispatch_failure() merges a failed taken reindex back into the
///   pending one (bounded for worker deaths by the requeue budget), unless
///   newer content already re-booked the work or the file's own run
///   crashed.
///
/// A TakenReindex carries the recording's sequence number as its identity; the
/// table never touches queues, sessions or workers.
class PendingTable {
public:
    struct TakenReindex {
        Fid id;
        std::uint64_t seq = 0;
    };

    /// Why a dispatch failed.
    enum class Failure : std::uint8_t {
        /// The run killed its worker (the worker named it): the same
        /// bytes would again, so the file waits for a change.
        Crashed,
        /// The worker died under it for some other reason — a death that
        /// named nothing, another request's crash: requeue on the budget.
        Lost,
        /// Preempted, or an outage the pool revives from: says nothing
        /// about the file, requeue freely.
        Preempted,
    };

    /// What a failed dispatch did to the file's pending state.
    enum class FailureDecision : std::uint8_t {
        /// No pending entry survived (file removed mid-flight).
        Dropped,
        /// The failed dispatch carried bytes older than the pending
        /// content; the newer content's own booking redoes the work.
        Outdated,
        /// The file's run crashed, or its requeue budget is spent; the
        /// file is abandoned and its table state cleared.
        GaveUp,
        /// The taken reindex merged back into the pending one for the next round.
        Requeued,
    };

    struct FailureOutcome {
        FailureDecision decision;
        /// Requeued into a fresh queue slot: the pump owes it a queue
        /// entry (a still-queued slot absorbs the requeue instead).
        bool needs_slot = false;
    };

    explicit PendingTable(unsigned max_requeue_attempts) :
        max_requeue_attempts(max_requeue_attempts) {}

    /// Record (or refresh) why a file is pending. Returns true when the
    /// file needs a fresh queue slot (none is queued-and-unconsumed).
    /// Every recording shields the entry from an in-flight attempt's
    /// finish; ContentChanged additionally starts a fresh crash budget —
    /// the crashes the old bytes caused say nothing about the fixed ones.
    bool record(Fid id, ReindexReason reason);

    /// Atomically take the file's pending reindex at dispatch, consuming
    /// its queued-slot state. Empty when the entry was cleared while the
    /// slot sat in the queue (file removed): the slot is skipped.
    std::optional<TakenReindex> take(Fid id);

    /// The TakenReindex a dispatch of the file's pending reindex would take, with
    /// nothing consumed — for waiters binding to the pending attempt.
    std::optional<TakenReindex> peek(Fid id) const;

    /// Finish the taken reindex after its attempt: erases the entry unless
    /// a recording during the flight booked a newer one, which survives.
    void finish(const TakenReindex& taken);

    /// The taken reindex's result no longer commits: the entry was cleared, or a
    /// ContentChanged recording overtook the launch — its bytes describe
    /// text that no longer exists. A deps-only recording is deliberately
    /// NOT outdating it: the in-flight index is positionally right, and
    /// suppressing it would trade a tolerated semantic drift for a
    /// coverage hole.
    bool outdated(const TakenReindex& taken) const;

    /// Merge a failed taken reindex back into the pending one. A crash of the
    /// file's own abandons it; only lost runs spend the bounded budget — a
    /// preemption says nothing about the file, and capping it would
    /// silently drop coverage. The requeue carries the reason the
    /// dispatch was launched for, not the entry's current one: a
    /// deps-only downgrade during the flight bet on the content pass
    /// finishing, and a failed pass leaves the edit uncovered.
    FailureOutcome on_dispatch_failure(const TakenReindex& taken, Failure failure);

    /// Why the file awaits re-indexing (queued or in flight), or nullopt
    /// when nothing is pending. O(1), no I/O.
    std::optional<ReindexReason> pending_reason(Fid id) const {
        auto it = entries.find(id);
        if(it == entries.end()) {
            return std::nullopt;
        }
        return it->second.reason;
    }

    bool contains(Fid id) const {
        return entries.count(id);
    }

    /// Whether an attempt took the file's pending reindex and runs still, with no
    /// newer recording queued after it.
    bool in_flight(Fid id) const {
        return entries.count(id) && !queued.contains(id);
    }

    /// Forget a file's pending reindex (file removed from disk, or given up):
    /// nothing is left to reindex, and a lingering ContentChanged reason
    /// would suppress its deliberately still-serving shard forever. A slot
    /// still in the queue keeps its mark: dispatch skips it, and a file
    /// recorded again before then reuses it — a second slot would dispatch
    /// the file twice in one round.
    void clear(Fid id) {
        entries.erase(id);
    }

    /// Every file with a pending reindex.
    llvm::SmallVector<Fid> pending_files() const;

    /// No pending reindex and no queued slots.
    bool empty() const {
        return entries.empty() && queued.empty();
    }

    /// Some file still holds a queued-and-unconsumed slot. The drained
    /// queue's compaction invariant: a pending reindex legitimately outlives the round
    /// (a round the graph refused keeps its booking), a queued slot must
    /// not.
    bool has_queued_slots() const {
        return !queued.empty();
    }

private:
    friend struct testing::IndexerFixture;

    struct Entry {
        ReindexReason reason;
        std::uint64_t seq;
        /// The sequence number of the newest ContentChanged recording. outdated()
        /// compares against this, not `seq`: a deps-only requeue
        /// during a flight bumps `seq` (to survive the finish) but
        /// must not discard an in-flight content pass.
        std::uint64_t content_seq;

        /// Lost-run requeues of this entry. Bounds the damage of a file
        /// whose runs keep dying without the worker naming it (an OOM
        /// kill): without a cap, every requeue would burn another worker.
        unsigned requeue_attempts = 0;
    };

    llvm::DenseMap<Fid, Entry> entries;

    /// Files holding a queued-and-unconsumed slot, for record()'s
    /// fresh-slot decision; the queue itself (ordering, rounds) belongs
    /// to the pump.
    llvm::DenseSet<Fid> queued;

    std::uint64_t seq = 0;
    unsigned max_requeue_attempts;
};

}  // namespace clice
