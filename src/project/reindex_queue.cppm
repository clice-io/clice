module clice:project.reindex_queue;

import :support.prelude;
import :vfs.ids;

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

/// The background index's queue of the files owed a reindex: the order they
/// are indexed in, why each is owed, and the take/finish contract of an
/// attempt:
///
/// - record() books a pending reindex and queues the file unless it holds a
///   slot not taken yet; within one queued slot ContentChanged absorbs,
///   while a fresh slot after consumption carries its own kind (the
///   consumed pass owns the earlier one).
/// - A round runs over the slots queued when it began (begin_round):
///   take_next() atomically takes the next file's pending reindex at
///   dispatch; events arriving during the flight book into the entry as a
///   newer one, and anything queued meanwhile waits for the next round.
/// - finish() consumes exactly the taken reindex: a newer recording
///   survives an older attempt's completion.
/// - on_dispatch_failure() merges a failed taken reindex back into the
///   pending one (bounded for worker deaths by the requeue budget), unless
///   newer content already re-booked the work or the file's own run
///   crashed.
///
/// A TakenReindex carries the recording's sequence number as its identity;
/// the queue never touches sessions or workers.
class ReindexQueue {
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
        /// file is abandoned and its pending state cleared.
        GaveUp,
        /// The taken reindex merged back into the pending one, queued for
        /// the next round.
        Requeued,
    };

    explicit ReindexQueue(unsigned max_requeue_attempts) :
        max_requeue_attempts(max_requeue_attempts) {}

    /// Record (or refresh) why a file is pending, queueing it when it holds
    /// no slot not taken yet. Every recording shields the entry from an
    /// in-flight attempt's finish; ContentChanged additionally starts a
    /// fresh crash budget — the crashes the old bytes caused say nothing
    /// about the fixed ones.
    void record(Fid id, ReindexReason reason);

    /// Move the file's slot to the front of the slots not taken yet. A slot
    /// past the running round stays past it and leads the next round: the
    /// round may have the file in flight, and a second attempt there would
    /// join the same node, where only one of the two commits an outcome.
    void move_to_front(Fid id);

    /// Start a round over every slot queued now, those `first` picks ahead
    /// of the others; how many slots it has. Anything queued during the
    /// round — its own failures' requeues included — waits for the next
    /// one: consuming a requeue in the round that produced it is what let a
    /// worker outage spin the dispatch loop against instant failures (#611).
    std::size_t begin_round(llvm::function_ref<bool(Fid)> first);

    /// Whether the running round has slots not taken yet.
    bool round_left() const {
        return position < round_end;
    }

    /// Take the next slot of the running round: its file's pending
    /// reindex. A slot whose entry was cleared while it sat in the queue
    /// (the file was removed) is skipped; none when the round has no slot
    /// left.
    std::optional<TakenReindex> take_next();

    /// The running round is over: its slots not taken stay for the next
    /// one, and a queue whose every slot was taken starts over empty.
    void end_round();

    /// The slots not taken yet.
    std::size_t queued() const {
        return slots.size() - position;
    }

    /// The TakenReindex a dispatch of the file's pending reindex would
    /// take, with nothing consumed — for waiters binding to the pending
    /// attempt.
    std::optional<TakenReindex> peek(Fid id) const;

    /// Finish the taken reindex after its attempt: erases the entry unless
    /// a recording during the flight booked a newer one, which survives.
    void finish(const TakenReindex& taken);

    /// The taken reindex's result no longer commits: the entry was cleared,
    /// or a ContentChanged recording overtook the launch — its bytes
    /// describe text that no longer exists. A deps-only recording is
    /// deliberately NOT outdating it: the in-flight index is positionally
    /// right, and suppressing it would trade a tolerated semantic drift for
    /// a coverage hole.
    bool outdated(const TakenReindex& taken) const;

    /// Merge a failed taken reindex back into the pending one. A crash of
    /// the file's own abandons it; only lost runs spend the bounded budget
    /// — a preemption says nothing about the file, and capping it would
    /// silently drop coverage. The requeue carries the reason the dispatch
    /// was launched for, not the entry's current one: a deps-only
    /// downgrade during the flight bet on the content pass finishing, and a
    /// failed pass leaves the edit uncovered.
    FailureDecision on_dispatch_failure(const TakenReindex& taken, Failure failure);

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

    /// Whether an attempt took the file's pending reindex and runs still,
    /// with no newer recording queued after it.
    bool in_flight(Fid id) const {
        return entries.count(id) && !queued_files.contains(id);
    }

    /// Forget a file's pending reindex (file removed from disk, or given
    /// up): nothing is left to reindex, and a lingering ContentChanged
    /// reason would suppress its deliberately still-serving shard forever.
    /// A slot still in the queue stays: take_next skips it, and a file
    /// recorded again before then reuses it — a second slot would dispatch
    /// the file twice in one round.
    void clear(Fid id) {
        entries.erase(id);
    }

private:
    friend struct testing::IndexerFixture;

    struct Entry {
        ReindexReason reason;
        std::uint64_t seq;
        /// The sequence number of the newest ContentChanged recording.
        /// outdated() compares against this, not `seq`: a deps-only requeue
        /// during a flight bumps `seq` (to survive the finish) but must not
        /// discard an in-flight content pass.
        std::uint64_t content_seq;

        /// Lost-run requeues of this entry. Bounds the damage of a file
        /// whose runs keep dying without the worker naming it (an OOM
        /// kill): without a cap, every requeue would burn another worker.
        unsigned requeue_attempts = 0;
    };

    llvm::DenseMap<Fid, Entry> entries;

    /// The files in the order they are indexed; those before `position`
    /// are taken.
    std::vector<Fid> slots;

    std::size_t position = 0;

    /// Where the running round's slots end; `position` once it is over.
    std::size_t round_end = 0;

    /// The files holding a slot not taken yet, for record()'s fresh-slot
    /// decision.
    llvm::DenseSet<Fid> queued_files;

    std::uint64_t seq = 0;

    unsigned max_requeue_attempts;
};

}  // namespace clice
