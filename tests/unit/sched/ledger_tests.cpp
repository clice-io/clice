#include "test/test.h"
#include "sched/index/ledger.h"

namespace clice::testing {
namespace {

using Verdict = PendingLedger::FailureVerdict;
using Failure = PendingLedger::Failure;

/// The claim/settle contract in isolation: debt is claimed atomically at
/// dispatch, flight-time recordings book newer debt, success consumes
/// only the claim, and failure merges the claim back — bounded for
/// crashes.
ZEST_SUITE(PendingLedger) {

constexpr static unsigned budget = 3;

clice::PendingLedger ledger{budget};

ZEST_CASE(content_absorbs_queued) {
    // Within one queued slot ContentChanged absorbs: a deps-only cascade
    // cannot downgrade a file whose own content already changed, and no
    // second slot is owed.
    ZEXPECT(ledger.record(Fid{1}, ReindexReason::ContentChanged));
    ZEXPECT(!ledger.record(Fid{1}, ReindexReason::DepsOnly));
    ZEXPECT(ledger.pending_reason(Fid{1}) == ReindexReason::ContentChanged);

    ZEXPECT(!ledger.record(Fid{1}, ReindexReason::ContentChanged));
    ZEXPECT(ledger.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
}

ZEST_CASE(consumed_pass_owns_debt) {
    // A deps-only recording after the slot was claimed is new debt of its
    // own kind: the in-flight pass owns the earlier content change, and
    // keeping ContentChanged would suppress the file's rows past it.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto claim = ledger.claim(Fid{1});
    ZASSERT(claim);

    ZEXPECT(ledger.record(Fid{1}, ReindexReason::DepsOnly));
    ZEXPECT(ledger.pending_reason(Fid{1}) == ReindexReason::DepsOnly);
}

ZEST_CASE(settle_spares_newer_debt) {
    // A recording during the flight books newer debt; the older attempt's
    // settle must leave it standing, and only the newer claim clears it.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto old_claim = ledger.claim(Fid{1});
    ZASSERT(old_claim);
    ledger.record(Fid{1}, ReindexReason::ContentChanged);

    ledger.settle(*old_claim);
    ZEXPECT(ledger.pending_reason(Fid{1}));

    auto fresh = ledger.claim(Fid{1});
    ZASSERT(fresh);
    ledger.settle(*fresh);
    ZEXPECT(!ledger.pending_reason(Fid{1}).has_value());
    ZEXPECT(ledger.empty());
}

ZEST_CASE(deps_never_supersede) {
    // Only newer content supersedes an in-flight pass: its rows describe
    // text that no longer exists. A deps-only recording does not — the
    // rows are positionally right and the follow-up covers the drift.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto claim = ledger.claim(Fid{1});
    ZASSERT(claim);
    ZEXPECT(!ledger.superseded(*claim));

    ledger.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(!ledger.superseded(*claim));

    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    ZEXPECT(ledger.superseded(*claim));

    // A cleared entry (file removed) also lands nothing.
    ledger.clear(Fid{1});
    ZEXPECT(ledger.superseded(*claim));
}

ZEST_CASE(superseded_failure_spends_nothing) {
    // A failed dispatch of superseded bytes neither requeues nor spends
    // the fresh content's crash budget: the newer booking redoes the
    // work with a full budget of its own.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto stale = ledger.claim(Fid{1});
    ZASSERT(stale);
    ledger.record(Fid{1}, ReindexReason::ContentChanged);

    ZEXPECT(ledger.on_dispatch_failure(*stale, Failure::Lost).verdict == Verdict::Superseded);

    // The fresh claim still has the whole budget: `budget` crashes
    // requeue before the next one abandons.
    for(unsigned i = 0; i < budget; i += 1) {
        auto claim = ledger.claim(Fid{1});
        ZASSERT(claim);
        auto outcome = ledger.on_dispatch_failure(*claim, Failure::Lost);
        ZEXPECT(outcome.verdict == Verdict::Requeued);
        ZEXPECT(outcome.needs_slot);
    }
    auto last = ledger.claim(Fid{1});
    ZASSERT(last);
    ZEXPECT(ledger.on_dispatch_failure(*last, Failure::Lost).verdict == Verdict::GaveUp);
    ZEXPECT(ledger.empty());
}

ZEST_CASE(preemption_needs_no_budget) {
    // Preemptions say nothing about the file: they requeue past any spent
    // budget — capping them would silently drop coverage.
    ledger.record(Fid{1}, ReindexReason::DepsOnly);
    for(int i = 0; i < 10; i += 1) {
        auto claim = ledger.claim(Fid{1});
        ZASSERT(claim);
        ZEXPECT(ledger.on_dispatch_failure(*claim, Failure::Preempted).verdict ==
                Verdict::Requeued);
    }
}

ZEST_CASE(requeue_carries_content) {
    // The requeue carries the debt class the dispatch was launched for: a
    // deps-only downgrade during the flight bet on the content pass
    // landing, and a failed pass leaves the edit uncovered.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto claim = ledger.claim(Fid{1});
    ZASSERT(claim);
    ledger.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(ledger.pending_reason(Fid{1}) == ReindexReason::DepsOnly);

    ZEXPECT(ledger.on_dispatch_failure(*claim, Failure::Lost).verdict == Verdict::Requeued);
    ZEXPECT(ledger.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
}

ZEST_CASE(own_crash_gives_up) {
    // A run that crashed its worker would crash the retry too: no requeue,
    // the file waits for its content to change.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto claim = ledger.claim(Fid{1});
    ZASSERT(claim);
    ZEXPECT(ledger.on_dispatch_failure(*claim, Failure::Crashed).verdict == Verdict::GaveUp);
    ZEXPECT(ledger.empty());
}

ZEST_CASE(cleared_claim_drops) {
    // A file removed mid-flight has nothing to redo.
    ledger.record(Fid{1}, ReindexReason::ContentChanged);
    auto claim = ledger.claim(Fid{1});
    ZASSERT(claim);
    ledger.clear(Fid{1});
    ZEXPECT(ledger.on_dispatch_failure(*claim, Failure::Lost).verdict == Verdict::Dropped);
    ZEXPECT(ledger.empty());
}

};  // ZEST_SUITE(PendingLedger)

}  // namespace
}  // namespace clice::testing
