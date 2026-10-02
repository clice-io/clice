#include <chrono>
#include <cstdint>
#include <string>

#include "test/test.h"
#include "sched/crash_budget.h"
#include "server/quarantine.h"

namespace clice::testing {

namespace {

using Clock = Quarantine::Clock;
using std::chrono::seconds;

constexpr std::uint8_t compile = 0;
constexpr std::uint8_t hover = 1;

TEST_SUITE(QuarantineMachine) {

TEST_CASE(CrashBarsItsKind) {
    Quarantine q;
    auto t0 = Clock::now();
    EXPECT_FALSE(q.barred(compile, t0));

    q.on_crash(compile, "d1", "killed by signal 11", t0);
    EXPECT_TRUE(q.barred(compile, t0));
    EXPECT_TRUE(q.crashed(compile));
    EXPECT_FALSE(q.barred(hover, t0));

    // A document that sits still is never retried.
    EXPECT_TRUE(q.barred(compile, t0 + seconds(3600)));
}

TEST_CASE(ChangeRetriesAfterSpacing) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_change(t0 + seconds(1));
    EXPECT_TRUE(q.barred(compile, t0 + seconds(1)));
    EXPECT_FALSE(q.barred(compile, t0 + Quarantine::retry_spacing));
}

TEST_CASE(StrikesLeaveSaveOnly) {
    Quarantine q;
    auto t = Clock::now();
    for(unsigned strike = 1; strike <= Quarantine::max_strikes; strike += 1) {
        q.on_crash(compile, std::to_string(strike), "cause", t);
        q.on_change(t);
        t += seconds(10);
    }
    EXPECT_TRUE(q.barred(compile, t));
    ASSERT_EQ(q.notes().size(), 1u);
    EXPECT_TRUE(q.notes()[0].save_only);

    q.on_save();
    EXPECT_FALSE(q.barred(compile, t));

    // The save starts a fresh run: one crash later, a change retries again.
    q.on_crash(compile, "after-save", "cause", t);
    q.on_change(t);
    EXPECT_FALSE(q.barred(compile, t + seconds(10)));
}

TEST_CASE(LandClearsRecord) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_land(compile);
    EXPECT_FALSE(q.crashed(compile));
    EXPECT_FALSE(q.barred(compile, t0));
    EXPECT_TRUE(q.empty());
}

TEST_CASE(DeathCountsOnce) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(hover, "d1", "cause", t0);
    q.on_crash(hover, "d1", "cause", t0);
    ASSERT_EQ(q.notes().size(), 1u);
    EXPECT_EQ(q.notes()[0].strikes, 1u);

    // Anonymous evidence always counts.
    q.on_crash(hover, "", "cause", t0);
    q.on_crash(hover, "", "cause", t0);
    EXPECT_EQ(q.notes()[0].strikes, 3u);
}

TEST_CASE(AttemptSpendsLicense) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_save();
    {
        Quarantine::Attempt attempt(q, compile);
        EXPECT_TRUE(q.barred(compile, t0));
    }
    // Ended without a crash: the license comes back.
    EXPECT_FALSE(q.barred(compile, t0));

    {
        Quarantine::Attempt attempt(q, compile);
        q.on_crash(compile, "d2", "cause", t0);
    }
    EXPECT_TRUE(q.barred(compile, t0 + seconds(10)));
}

TEST_CASE(ChangeDuringFlightStands) {
    // The crash describes the inputs the attempt carried; an edit during
    // its flight is still untried.
    Quarantine q;
    auto t0 = Clock::now();
    {
        Quarantine::Attempt attempt(q, compile);
        q.on_change(t0);
        q.on_crash(compile, "d1", "cause", t0 + seconds(20));
    }
    EXPECT_FALSE(q.barred(compile, t0 + seconds(30)));
}

TEST_CASE(EditingCrashStaysSilent) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_change(t0);
    q.on_crash(compile, "d1", "cause", t0 + seconds(1));
    EXPECT_FALSE(q.shows(compile));
    EXPECT_TRUE(q.notes().empty());

    // A repeat shows.
    q.on_change(t0 + seconds(2));
    q.on_crash(compile, "d2", "cause", t0 + seconds(5));
    EXPECT_TRUE(q.shows(compile));
    EXPECT_EQ(q.notes().size(), 1u);
}

TEST_CASE(ColdCrashShows) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_change(t0);
    q.on_crash(hover, "d1", "killed by signal 6 (SIGABRT)", t0 + Quarantine::editing_window);
    ASSERT_EQ(q.notes().size(), 1u);
    EXPECT_EQ(q.notes()[0].kind, hover);
    EXPECT_EQ(q.notes()[0].cause, "killed by signal 6 (SIGABRT)");
    EXPECT_FALSE(q.notes()[0].save_only);
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
