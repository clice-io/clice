module;

#include "modules/prelude.h"

module clice;

import :sched.index.pending_table;
import :unittests.utils;

namespace clice::testing {
namespace {

using Decision = PendingTable::FailureDecision;
using Failure = PendingTable::Failure;

/// The claim/settle contract in isolation: debt is claimed atomically at
/// dispatch, flight-time recordings book newer debt, success consumes
/// only the claim, and failure merges the claim back — bounded for
/// crashes.
ZEST_SUITE(PendingTable) {

constexpr static unsigned budget = 3;

clice::PendingTable table{budget};

ZEST_CASE(content_absorbs_queued) {
    // Within one queued slot ContentChanged absorbs: a deps-only cascade
    // cannot downgrade a file whose own content already changed, and no
    // second slot is owed.
    ZEXPECT(table.record(Fid{1}, ReindexReason::ContentChanged));
    ZEXPECT(!table.record(Fid{1}, ReindexReason::DepsOnly));
    ZEXPECT(table.pending_reason(Fid{1}) == ReindexReason::ContentChanged);

    ZEXPECT(!table.record(Fid{1}, ReindexReason::ContentChanged));
    ZEXPECT(table.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
}

ZEST_CASE(consumed_pass_owns_debt) {
    // A deps-only recording after the slot was claimed is new debt of its
    // own kind: the in-flight pass owns the earlier content change, and
    // keeping ContentChanged would suppress the file's rows past it.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = table.take(Fid{1});
    ZASSERT(taken);

    ZEXPECT(table.record(Fid{1}, ReindexReason::DepsOnly));
    ZEXPECT(table.pending_reason(Fid{1}) == ReindexReason::DepsOnly);
}

ZEST_CASE(finish_spares_newer_debt) {
    // A recording during the flight books newer debt; the older attempt's
    // settle must leave it standing, and only the newer claim clears it.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto old_taken = table.take(Fid{1});
    ZASSERT(old_taken);
    table.record(Fid{1}, ReindexReason::ContentChanged);

    table.finish(*old_taken);
    ZEXPECT(table.pending_reason(Fid{1}));

    auto fresh = table.take(Fid{1});
    ZASSERT(fresh);
    table.finish(*fresh);
    ZEXPECT(!table.pending_reason(Fid{1}).has_value());
    ZEXPECT(table.empty());
}

ZEST_CASE(deps_never_supersede) {
    // Only newer content supersedes an in-flight pass: its rows describe
    // text that no longer exists. A deps-only recording does not — the
    // rows are positionally right and the follow-up covers the drift.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = table.take(Fid{1});
    ZASSERT(taken);
    ZEXPECT(!table.superseded(*taken));

    table.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(!table.superseded(*taken));

    table.record(Fid{1}, ReindexReason::ContentChanged);
    ZEXPECT(table.superseded(*taken));

    // A cleared entry (file removed) also lands nothing.
    table.clear(Fid{1});
    ZEXPECT(table.superseded(*taken));
}

ZEST_CASE(superseded_failure_spends_nothing) {
    // A failed dispatch of superseded bytes neither requeues nor spends
    // the fresh content's crash budget: the newer booking redoes the
    // work with a full budget of its own.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto stale = table.take(Fid{1});
    ZASSERT(stale);
    table.record(Fid{1}, ReindexReason::ContentChanged);

    ZEXPECT(table.on_dispatch_failure(*stale, Failure::Lost).decision == Decision::Superseded);

    // The fresh claim still has the whole budget: `budget` crashes
    // requeue before the next one abandons.
    for(unsigned i = 0; i < budget; i += 1) {
        auto taken = table.take(Fid{1});
        ZASSERT(taken);
        auto outcome = table.on_dispatch_failure(*taken, Failure::Lost);
        ZEXPECT(outcome.decision == Decision::Requeued);
        ZEXPECT(outcome.needs_slot);
    }
    auto last = table.take(Fid{1});
    ZASSERT(last);
    ZEXPECT(table.on_dispatch_failure(*last, Failure::Lost).decision == Decision::GaveUp);
    ZEXPECT(table.empty());
}

ZEST_CASE(preemption_needs_no_budget) {
    // Preemptions say nothing about the file: they requeue past any spent
    // budget — capping them would silently drop coverage.
    table.record(Fid{1}, ReindexReason::DepsOnly);
    for(int i = 0; i < 10; i += 1) {
        auto taken = table.take(Fid{1});
        ZASSERT(taken);
        ZEXPECT(table.on_dispatch_failure(*taken, Failure::Preempted).decision ==
                Decision::Requeued);
    }
}

ZEST_CASE(requeue_carries_content) {
    // The requeue carries the debt class the dispatch was launched for: a
    // deps-only downgrade during the flight bet on the content pass
    // landing, and a failed pass leaves the edit uncovered.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = table.take(Fid{1});
    ZASSERT(taken);
    table.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(table.pending_reason(Fid{1}) == ReindexReason::DepsOnly);

    ZEXPECT(table.on_dispatch_failure(*taken, Failure::Lost).decision == Decision::Requeued);
    ZEXPECT(table.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
}

ZEST_CASE(own_crash_gives_up) {
    // A run that crashed its worker would crash the retry too: no requeue,
    // the file waits for its content to change.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = table.take(Fid{1});
    ZASSERT(taken);
    ZEXPECT(table.on_dispatch_failure(*taken, Failure::Crashed).decision == Decision::GaveUp);
    ZEXPECT(table.empty());
}

ZEST_CASE(cleared_take_drops) {
    // A file removed mid-flight has nothing to redo.
    table.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = table.take(Fid{1});
    ZASSERT(taken);
    table.clear(Fid{1});
    ZEXPECT(table.on_dispatch_failure(*taken, Failure::Lost).decision == Decision::Dropped);
    ZEXPECT(table.empty());
}

ZEST_CASE(cleared_slot_reused) {
    // A file removed from disk and recreated before its slot was consumed
    // owes no second slot: both would dispatch it in one round.
    ZEXPECT(table.record(Fid{1}, ReindexReason::ContentChanged));
    table.clear(Fid{1});
    ZEXPECT(!table.record(Fid{1}, ReindexReason::ContentChanged));
    ZEXPECT(table.take(Fid{1}).has_value());
    ZEXPECT(!table.has_queued_slots());
}

};  // ZEST_SUITE(PendingTable)

}  // namespace
}  // namespace clice::testing
