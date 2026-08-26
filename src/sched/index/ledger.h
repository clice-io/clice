#pragma once

#include <cstdint>
#include <optional>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace clice {

namespace testing {

struct IndexerFixture;

}

/// Why a file awaits re-indexing. The invalidation engine knows the cause
/// at enqueue time, so queries can decide in O(1) whether a pending file's
/// existing index rows are still trustworthy (see IndexQuery's freshness
/// contract).
enum class ReindexReason : std::uint8_t {
    /// Enqueued by a dependency cascade (or a bulk sweep of unknown
    /// staleness): the file's own content is not known to have changed, so
    /// its index rows are positionally intact — at worst semantically
    /// behind — and keep serving until the reindex lands.
    DepsOnly,
    /// The file's own content changed: its index rows describe text that
    /// no longer exists, so queries skip this file's contribution until
    /// the reindex lands.
    ContentChanged,
};

/// Dispatch/landing-time admission verdict on one unit of pump work,
/// produced by the client (the serving side knows sessions; batch is
/// trivially Admit) — the pump itself never sees a SessionStore.
enum class Admission : std::uint8_t {
    /// Run the work as planned.
    Admit,
    /// Skip the work and settle the claimed debt: an ordinary open
    /// session's veto must clear the debt, or the pump spins and a
    /// ContentChanged suppression hangs for the whole open period.
    SkipAndSettle,
    /// Skip the work and keep the debt for a later round.
    Defer,
};

/// The pump's per-file debt ledger — the claim/settle contract behind
/// background reindexing:
///
/// - record() books debt; within one queued slot ContentChanged absorbs,
///   while a fresh slot after consumption carries its own kind (the
///   consumed pass owns the earlier debt).
/// - claim() atomically takes the file's current debt at dispatch;
///   events landing during the flight book into the entry as newer debt.
/// - settle() consumes exactly the claimed debt: a newer recording
///   survives an older attempt's completion.
/// - on_dispatch_failure() merges a failed claim back into pending debt
///   (bounded for crashes by the requeue budget), unless newer content
///   already re-booked the work.
///
/// Claims carry an opaque monotonic ticket as their identity; the ledger
/// never touches queues, sessions or workers.
class PendingLedger {
public:
    struct Claim {
        std::uint32_t id = 0;
        std::uint64_t ticket = 0;
    };

    /// What a failed dispatch did to the file's pending state.
    enum class FailureVerdict : std::uint8_t {
        /// No pending entry survived (file removed mid-flight).
        Dropped,
        /// The failed dispatch carried bytes older than the pending
        /// content; the newer content's own booking redoes the work.
        Superseded,
        /// The crash budget is spent; the file is abandoned and its
        /// ledger state cleared.
        GaveUp,
        /// The claim merged back into pending debt for the next round.
        Requeued,
    };

    struct FailureOutcome {
        FailureVerdict verdict;
        /// Requeued into a fresh queue slot: the pump owes it a queue
        /// entry (a still-queued slot absorbs the requeue instead).
        bool needs_slot = false;
    };

    explicit PendingLedger(unsigned max_requeue_attempts) :
        max_requeue_attempts(max_requeue_attempts) {}

    /// Record (or refresh) why a file is pending. Returns true when the
    /// file needs a fresh queue slot (none is queued-and-unconsumed).
    /// Every recording shields the entry from an in-flight attempt's
    /// settle; ContentChanged additionally starts a fresh crash budget —
    /// the crashes the old bytes caused say nothing about the fixed ones.
    bool record(std::uint32_t id, ReindexReason reason);

    /// Atomically claim the file's current debt at dispatch, consuming
    /// its queued-slot state. Empty when the entry was cleared while the
    /// slot sat in the queue (file removed): the slot is skipped.
    std::optional<Claim> claim(std::uint32_t id);

    /// The claim a dispatch of the file's current debt would take, with
    /// nothing consumed — for waiters binding to the pending attempt.
    std::optional<Claim> peek(std::uint32_t id) const;

    /// Settle the claimed debt after its attempt: erases the entry unless
    /// a recording during the flight booked newer debt, which survives.
    void settle(const Claim& claim);

    /// The claim's result no longer lands: the entry was cleared, or a
    /// ContentChanged recording overtook the launch — its bytes describe
    /// text that no longer exists. A deps-only recording is deliberately
    /// NOT superseding: in-flight rows are positionally right, and
    /// suppressing them would trade a tolerated semantic drift for a
    /// coverage hole.
    bool superseded(const Claim& claim) const;

    /// Merge a failed claim back into pending debt. Only crashes spend
    /// the bounded budget — a preemption says nothing about the file, and
    /// capping it would silently drop coverage. The requeue carries the
    /// debt class the dispatch was launched for, not the entry's current
    /// one: a deps-only downgrade during the flight bet on the content
    /// pass landing, and a failed pass leaves the edit uncovered.
    FailureOutcome on_dispatch_failure(const Claim& claim, bool crashed);

    /// Why the file awaits re-indexing (queued or in flight), or nullopt
    /// when nothing is pending. O(1), no I/O.
    std::optional<ReindexReason> pending_reason(std::uint32_t id) const {
        auto it = entries.find(id);
        if(it == entries.end()) {
            return std::nullopt;
        }
        return it->second.reason;
    }

    bool contains(std::uint32_t id) const {
        return entries.count(id);
    }

    /// Forget a file's debt and queued-slot state (file removed from
    /// disk): nothing is left to reindex, and a lingering ContentChanged
    /// reason would suppress its deliberately still-serving shard
    /// forever.
    void clear(std::uint32_t id) {
        entries.erase(id);
        queued.erase(id);
    }

    /// Every file with booked debt (batch debt reporting).
    llvm::SmallVector<std::uint32_t> pending_files() const;

    /// No debt and no queued slots.
    bool empty() const {
        return entries.empty() && queued.empty();
    }

    /// Some file still holds a queued-and-unconsumed slot. The drained
    /// queue's compaction invariant: deferred debt (admission's Defer)
    /// legitimately outlives the round, a queued slot must not.
    bool has_queued_slots() const {
        return !queued.empty();
    }

private:
    friend struct testing::IndexerFixture;

    struct Entry {
        ReindexReason reason;
        std::uint64_t ticket;
        /// Ticket of the newest ContentChanged recording. superseded()
        /// compares against this, not `ticket`: a deps-only requeue
        /// during a flight bumps `ticket` (to survive the settle) but
        /// must not discard an in-flight content pass.
        std::uint64_t content_ticket;

        /// Crash requeues of this entry. Bounds the damage of a poison
        /// file that reliably crashes workers: without a cap, every
        /// requeue would burn another worker's crash budget until the
        /// whole pool is dead.
        unsigned requeue_attempts = 0;
    };

    llvm::DenseMap<std::uint32_t, Entry> entries;

    /// Files holding a queued-and-unconsumed slot, for record()'s
    /// fresh-slot decision; the queue itself (ordering, rounds) belongs
    /// to the pump.
    llvm::DenseSet<std::uint32_t> queued;

    std::uint64_t ticket = 0;
    unsigned max_requeue_attempts;
};

}  // namespace clice
