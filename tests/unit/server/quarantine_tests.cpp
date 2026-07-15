#include <chrono>

#include "test/test.h"
#include "server/state/quarantine.h"

namespace clice::testing {

namespace {

TEST_SUITE(QuarantineMachine) {

TEST_CASE(CrashesAccumulate) {
    Quarantine q;
    EXPECT_FALSE(q.active());

    q.on_crash();
    EXPECT_EQ(q.crashes(), 1u);
    EXPECT_FALSE(q.active());
    EXPECT_FALSE(q.blocked());

    q.on_crash();
    EXPECT_EQ(q.crashes(), 2u);
    EXPECT_TRUE(q.active());
    EXPECT_TRUE(q.blocked());
}

TEST_CASE(LandClearsInherited) {
    Quarantine q;
    q.on_crash();

    // A crash recorded while the flight was up survives its landing: the
    // same content will kill again on the next request.
    auto flight = q.begin_flight();
    q.on_crash();
    EXPECT_TRUE(q.grew(flight));
    q.land(flight);
    EXPECT_EQ(q.crashes(), 1u);

    // A clean flight clears everything it inherited.
    auto clean = q.begin_flight();
    EXPECT_FALSE(q.grew(clean));
    q.land(clean);
    EXPECT_EQ(q.crashes(), 0u);
}

TEST_CASE(LandNeverUnderflows) {
    Quarantine q;
    q.on_crash();
    auto flight = q.begin_flight();

    // A reopen (reset) while the flight is up must not underflow the
    // fresh record when the stale flight lands.
    q.reset();
    q.land(flight);
    EXPECT_EQ(q.crashes(), 0u);
}

TEST_CASE(EditArmsOneProbe) {
    Quarantine q;

    // Below the threshold an edit grants nothing.
    q.on_crash();
    q.on_edit(true);
    EXPECT_FALSE(q.blocked());
    q.on_crash();
    EXPECT_TRUE(q.blocked());

    // Dropped and no-op edits grant nothing; a real one arms the probe.
    q.on_edit(false);
    EXPECT_TRUE(q.blocked());
    q.on_edit(true);
    EXPECT_FALSE(q.blocked());
    EXPECT_TRUE(q.active());

    // The attempt spends it; back to blocked. Re-arming is only honored
    // while quarantined — a stray re-arm below the threshold must not
    // pre-arm a future spell.
    q.spend_probe();
    EXPECT_TRUE(q.blocked());
    q.land(q.begin_flight());
    q.re_arm_probe();
    q.on_crash();
    q.on_crash();
    EXPECT_TRUE(q.blocked());
    q.on_edit(true);

    // An attempt that never ran hands the license back.
    q.on_edit(true);
    q.spend_probe();
    q.re_arm_probe();
    EXPECT_FALSE(q.blocked());
}

TEST_CASE(ProbeSuccessRecovers) {
    Quarantine q;
    q.on_crash();
    q.on_crash();
    q.on_edit(true);

    // The probe compile: takes off, spends the probe, lands clean.
    auto flight = q.begin_flight();
    q.spend_probe();
    q.land(flight);
    EXPECT_EQ(q.crashes(), 0u);
    EXPECT_FALSE(q.active());
    EXPECT_FALSE(q.blocked());

    // Recovery leaves no stale probe: the next spell blocks immediately
    // instead of inheriting a free attempt from the last one.
    q.on_crash();
    q.on_crash();
    EXPECT_TRUE(q.blocked());
}

TEST_CASE(QueryCrashSurvivesCompile) {
    Quarantine q;
    q.on_kind_crash(1);

    // A successful compile does not disprove a kind crash: the same AST
    // crashes the same query again. Without the separate ledger, the
    // crash-triggered recompile would launder the evidence forever.
    q.land(q.begin_flight());
    EXPECT_EQ(q.crashes(), 1u);
    q.on_kind_crash(1);
    EXPECT_TRUE(q.blocked());

    // Recovery: an edit grants the probe, its compile lands, and the
    // crashing kind answering clears the ledger.
    q.on_edit(true);
    q.spend_probe();
    q.land(q.begin_flight());
    EXPECT_TRUE(q.active());
    q.on_kind_land(1);
    EXPECT_FALSE(q.active());
    EXPECT_FALSE(q.blocked());
}

TEST_CASE(KindsRecoverIndependently) {
    Quarantine q;
    q.on_kind_crash(1);
    q.on_kind_crash(1);
    EXPECT_TRUE(q.blocked());

    // A harmless other kind answering (a hover while semantic tokens is
    // the crasher) must not launder the crashing kind's evidence.
    q.on_kind_land(2);
    EXPECT_TRUE(q.blocked());

    q.on_kind_land(1);
    EXPECT_FALSE(q.active());

    // Leaving quarantine via a kind landing retires an armed probe too —
    // an adoption (PCH cache hit) can land a kind before any dispatch
    // spent the probe, and the next spell must not inherit it.
    q.on_edit(true);
    q.on_kind_crash(3);
    q.on_kind_crash(3);
    EXPECT_TRUE(q.blocked());
}

TEST_CASE(MixedEvidenceCounts) {
    Quarantine q;
    q.on_crash();

    // Kind evidence accrued mid-flight counts toward grew(); the landing
    // clears only the inherited compile share.
    auto flight = q.begin_flight();
    q.on_kind_crash(1);
    EXPECT_TRUE(q.grew(flight));
    q.land(flight);
    EXPECT_EQ(q.crashes(), 1u);

    q.on_kind_land(1);
    EXPECT_EQ(q.crashes(), 0u);
}

TEST_CASE(DeathCountedOnce) {
    Quarantine q;

    // One process death fails every request in flight on it: a compile and
    // a query blaming the same incarnation count once, across ledgers.
    q.on_crash("sf:1:7");
    q.on_kind_crash(1, "sf:1:7");
    EXPECT_EQ(q.crashes(), 1u);

    // A different incarnation is a different death.
    q.on_crash("sf:1:8");
    EXPECT_EQ(q.crashes(), 2u);

    // Anonymous evidence (no identity) always counts.
    q.on_crash();
    q.on_crash();
    EXPECT_EQ(q.crashes(), 4u);
}

TEST_CASE(ProbeRidesCrashingKind) {
    Quarantine q;
    q.on_kind_crash(1);
    q.on_kind_crash(1);
    q.on_edit(true);

    // Only the crashing kind's dispatch is the recovery attempt: a
    // harmless other kind or an innocent compile must not spend the probe.
    EXPECT_FALSE(q.recovery_compile());
    EXPECT_FALSE(q.recovery_kind(2));
    EXPECT_TRUE(q.recovery_kind(1));

    // Compile strikes make the compile the recovery vehicle.
    q.on_crash();
    EXPECT_TRUE(q.recovery_compile());

    // The predicates require the armed probe: a concurrent request that
    // lost the race to spend it holds no license, and the kind is simply
    // blocked again.
    q.spend_probe();
    EXPECT_FALSE(q.recovery_compile());
    EXPECT_FALSE(q.recovery_kind(1));
    EXPECT_TRUE(q.kind_blocked(1));
    EXPECT_FALSE(q.kind_blocked(2));
}

TEST_CASE(GuardReturnsUnspentProbe) {
    Quarantine q;
    q.on_kind_crash(1);
    q.on_kind_crash(1);
    q.on_edit(true);

    // Unwinding with no strike recorded (a cancellation before dispatch)
    // hands the license back.
    {
        Quarantine::ProbeGuard guard(q);
        EXPECT_TRUE(q.blocked());
    }
    EXPECT_FALSE(q.blocked());

    // A recorded strike keeps it spent: the attempt ran and crashed.
    {
        Quarantine::ProbeGuard guard(q);
        q.on_kind_crash(1);
    }
    EXPECT_TRUE(q.blocked());
}

TEST_CASE(AnnounceOncePerSpell) {
    Quarantine q;
    EXPECT_FALSE(q.needs_announcement());

    q.on_crash();
    q.on_crash();
    EXPECT_TRUE(q.needs_announcement());
    q.mark_announced();
    EXPECT_FALSE(q.needs_announcement());

    // More crashes in the same spell do not re-announce, and neither does
    // a landing that clears only part of the evidence and stays active.
    q.on_crash();
    EXPECT_FALSE(q.needs_announcement());
    {
        auto flight = q.begin_flight();
        q.on_crash();
        q.on_crash();
        q.land(flight);
        EXPECT_TRUE(q.active());
        EXPECT_FALSE(q.needs_announcement());
    }
    q.land(q.begin_flight());
    q.on_crash();
    q.on_crash();
    EXPECT_TRUE(q.needs_announcement());
}

TEST_CASE(ResetClearsAll) {
    Quarantine q;
    q.on_crash();
    q.on_crash();
    q.on_edit(true);
    q.mark_announced();

    q.on_kind_crash(1);
    q.reset();
    EXPECT_EQ(q.crashes(), 0u);
    EXPECT_FALSE(q.active());
    EXPECT_FALSE(q.blocked());
    EXPECT_FALSE(q.needs_announcement());

    // reset() must clear the probe and the announcement, not just the
    // streak: a fresh spell must block and announce again.
    q.on_crash();
    q.on_crash();
    EXPECT_TRUE(q.blocked());
    EXPECT_TRUE(q.needs_announcement());
}

TEST_CASE(BudgetBlocksAtThreshold) {
    CrashBudget budget;
    EXPECT_FALSE(budget.blocked("key-a"));

    budget.on_crash("key-a");
    EXPECT_FALSE(budget.blocked("key-a"));
    budget.on_crash("key-a");
    EXPECT_TRUE(budget.blocked("key-a"));

    // Keys are independent: fresh content (fresh key) starts fresh.
    EXPECT_FALSE(budget.blocked("key-b"));
}

TEST_CASE(BudgetClearsOnLand) {
    // A successful build proves the strikes were transient: without the
    // clear, two unrelated hiccups far apart would block a key that
    // rebuilds fine in between.
    CrashBudget budget;
    budget.on_crash("key");
    budget.on_land("key");
    budget.on_crash("key");
    EXPECT_FALSE(budget.blocked("key"));
}

TEST_CASE(BudgetRearmsAfterCooldown) {
    // The poison may live in content the key cannot see (a header included
    // by the hashed preamble text): a block is a cooldown, not a verdict.
    // Zero cooldown models "elapsed" — the key earns a fresh budget.
    CrashBudget budget{std::chrono::milliseconds(0)};
    budget.on_crash("key");
    budget.on_crash("key");
    EXPECT_FALSE(budget.blocked("key"));
}

};  // TEST_SUITE(QuarantineMachine)

}  // namespace

}  // namespace clice::testing
