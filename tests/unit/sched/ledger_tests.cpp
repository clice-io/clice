#include "test/test.h"
#include "sched/index/ledger.h"

namespace clice::testing {
namespace {

using Verdict = PendingLedger::FailureVerdict;

/// The claim/settle contract in isolation: debt is claimed atomically at
/// dispatch, flight-time recordings book newer debt, success consumes
/// only the claim, and failure merges the claim back — bounded for
/// crashes.
TEST_SUITE(PendingLedger) {

constexpr static unsigned budget = 3;

clice::PendingLedger ledger{budget};

TEST_CASE(content_absorbs_queued) {
    // Within one queued slot ContentChanged absorbs: a deps-only cascade
    // cannot downgrade a file whose own content already changed, and no
    // second slot is owed.
    EXPECT_TRUE(ledger.record(1, ReindexReason::ContentChanged));
    EXPECT_FALSE(ledger.record(1, ReindexReason::DepsOnly));
    EXPECT_TRUE(ledger.pending_reason(1) == ReindexReason::ContentChanged);

    EXPECT_FALSE(ledger.record(1, ReindexReason::ContentChanged));
    EXPECT_TRUE(ledger.pending_reason(1) == ReindexReason::ContentChanged);
}

TEST_CASE(consumed_pass_owns_debt) {
    // A deps-only recording after the slot was claimed is new debt of its
    // own kind: the in-flight pass owns the earlier content change, and
    // keeping ContentChanged would suppress the file's rows past it.
    ledger.record(1, ReindexReason::ContentChanged);
    auto claim = ledger.claim(1);
    ASSERT_TRUE(claim.has_value());

    EXPECT_TRUE(ledger.record(1, ReindexReason::DepsOnly));
    EXPECT_TRUE(ledger.pending_reason(1) == ReindexReason::DepsOnly);
}

TEST_CASE(settle_spares_newer_debt) {
    // A recording during the flight books newer debt; the older attempt's
    // settle must leave it standing, and only the newer claim clears it.
    ledger.record(1, ReindexReason::ContentChanged);
    auto old_claim = ledger.claim(1);
    ASSERT_TRUE(old_claim.has_value());
    ledger.record(1, ReindexReason::ContentChanged);

    ledger.settle(*old_claim);
    EXPECT_TRUE(ledger.pending_reason(1).has_value());

    auto fresh = ledger.claim(1);
    ASSERT_TRUE(fresh.has_value());
    ledger.settle(*fresh);
    EXPECT_FALSE(ledger.pending_reason(1).has_value());
    EXPECT_TRUE(ledger.empty());
}

TEST_CASE(deps_never_supersede) {
    // Only newer content supersedes an in-flight pass: its rows describe
    // text that no longer exists. A deps-only recording does not — the
    // rows are positionally right and the follow-up covers the drift.
    ledger.record(1, ReindexReason::ContentChanged);
    auto claim = ledger.claim(1);
    ASSERT_TRUE(claim.has_value());
    EXPECT_FALSE(ledger.superseded(*claim));

    ledger.record(1, ReindexReason::DepsOnly);
    EXPECT_FALSE(ledger.superseded(*claim));

    ledger.record(1, ReindexReason::ContentChanged);
    EXPECT_TRUE(ledger.superseded(*claim));

    // A cleared entry (file removed) also lands nothing.
    ledger.clear(1);
    EXPECT_TRUE(ledger.superseded(*claim));
}

TEST_CASE(superseded_failure_spends_nothing) {
    // A failed dispatch of superseded bytes neither requeues nor spends
    // the fresh content's crash budget: the newer booking redoes the
    // work with a full budget of its own.
    ledger.record(1, ReindexReason::ContentChanged);
    auto stale = ledger.claim(1);
    ASSERT_TRUE(stale.has_value());
    ledger.record(1, ReindexReason::ContentChanged);

    EXPECT_TRUE(ledger.on_dispatch_failure(*stale, true).verdict == Verdict::Superseded);

    // The fresh claim still has the whole budget: `budget` crashes
    // requeue before the next one abandons.
    for(unsigned i = 0; i < budget; i += 1) {
        auto claim = ledger.claim(1);
        ASSERT_TRUE(claim.has_value());
        auto outcome = ledger.on_dispatch_failure(*claim, true);
        EXPECT_TRUE(outcome.verdict == Verdict::Requeued);
        EXPECT_TRUE(outcome.needs_slot);
    }
    auto last = ledger.claim(1);
    ASSERT_TRUE(last.has_value());
    EXPECT_TRUE(ledger.on_dispatch_failure(*last, true).verdict == Verdict::GaveUp);
    EXPECT_TRUE(ledger.empty());
}

TEST_CASE(preemption_needs_no_budget) {
    // Preemptions say nothing about the file: they requeue past any spent
    // budget — capping them would silently drop coverage.
    ledger.record(1, ReindexReason::DepsOnly);
    for(int i = 0; i < 10; i += 1) {
        auto claim = ledger.claim(1);
        ASSERT_TRUE(claim.has_value());
        EXPECT_TRUE(ledger.on_dispatch_failure(*claim, false).verdict == Verdict::Requeued);
    }
}

TEST_CASE(requeue_carries_content) {
    // The requeue carries the debt class the dispatch was launched for: a
    // deps-only downgrade during the flight bet on the content pass
    // landing, and a failed pass leaves the edit uncovered.
    ledger.record(1, ReindexReason::ContentChanged);
    auto claim = ledger.claim(1);
    ASSERT_TRUE(claim.has_value());
    ledger.record(1, ReindexReason::DepsOnly);
    EXPECT_TRUE(ledger.pending_reason(1) == ReindexReason::DepsOnly);

    EXPECT_TRUE(ledger.on_dispatch_failure(*claim, true).verdict == Verdict::Requeued);
    EXPECT_TRUE(ledger.pending_reason(1) == ReindexReason::ContentChanged);
}

TEST_CASE(cleared_claim_drops) {
    // A file removed mid-flight has nothing to redo.
    ledger.record(1, ReindexReason::ContentChanged);
    auto claim = ledger.claim(1);
    ASSERT_TRUE(claim.has_value());
    ledger.clear(1);
    EXPECT_TRUE(ledger.on_dispatch_failure(*claim, true).verdict == Verdict::Dropped);
    EXPECT_TRUE(ledger.empty());
}

};  // TEST_SUITE(PendingLedger)

}  // namespace
}  // namespace clice::testing
