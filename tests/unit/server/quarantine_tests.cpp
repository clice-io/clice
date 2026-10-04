#include <chrono>
#include <cstdint>
#include <string>

#include "test/test.h"
#include "sched/blame_budget.h"
#include "server/quarantine.h"

namespace clice::testing {

namespace {

using Clock = Quarantine::Clock;
using std::chrono::seconds;

constexpr std::uint8_t compile = 0;
constexpr std::uint8_t hover = 1;

ZEST_SUITE(QuarantineMachine) {

ZEST_CASE(CrashBarsItsKind) {
    Quarantine q;
    auto t0 = Clock::now();
    EXPECT(!q.barred(compile, t0));

    q.on_crash(compile, "d1", "killed by signal 11", t0);
    EXPECT(q.barred(compile, t0));
    EXPECT(q.crashed(compile));
    EXPECT(!q.barred(hover, t0));

    // A document that sits still is never retried.
    EXPECT(q.barred(compile, t0 + seconds(3600)));
}

ZEST_CASE(ChangeRetriesAfterSpacing) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_change(t0 + seconds(1));
    EXPECT(q.barred(compile, t0 + seconds(1)));
    EXPECT(!q.barred(compile, t0 + Quarantine::retry_spacing));
}

ZEST_CASE(StrikesLeaveSaveOnly) {
    Quarantine q;
    auto t = Clock::now();
    for(unsigned strike = 1; strike <= Quarantine::max_strikes; strike += 1) {
        q.on_crash(compile, std::to_string(strike), "cause", t);
        q.on_change(t);
        t += seconds(10);
    }
    EXPECT(q.barred(compile, t));
    ASSERT(q.notes().size() == 1u);
    EXPECT(q.notes()[0].save_only);

    q.on_save();
    EXPECT(!q.barred(compile, t));

    // The save starts a fresh run: one crash later, a change retries again.
    q.on_crash(compile, "after-save", "cause", t);
    q.on_change(t);
    EXPECT(!q.barred(compile, t + seconds(10)));
}

ZEST_CASE(LandClearsRecord) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_land(compile);
    EXPECT(!q.crashed(compile));
    EXPECT(!q.barred(compile, t0));
    EXPECT(q.empty());
}

ZEST_CASE(DeathCountsOnce) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(hover, "d1", "cause", t0);
    q.on_crash(hover, "d1", "cause", t0);
    ASSERT(q.notes().size() == 1u);
    EXPECT(q.notes()[0].strikes == 1u);

    // Anonymous evidence always counts.
    q.on_crash(hover, "", "cause", t0);
    q.on_crash(hover, "", "cause", t0);
    EXPECT(q.notes()[0].strikes == 3u);
}

ZEST_CASE(AttemptSpendsLicense) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_save();
    {
        Quarantine::Attempt attempt(q, compile);
        EXPECT(q.barred(compile, t0));
    }
    // Ended without a crash: the license comes back.
    EXPECT(!q.barred(compile, t0));

    {
        Quarantine::Attempt attempt(q, compile);
        q.on_crash(compile, "d2", "cause", t0);
    }
    EXPECT(q.barred(compile, t0 + seconds(10)));
}

ZEST_CASE(SaveThenCrashKeepsBar) {
    // The strikes a save reset climb back to the count the attempt saw;
    // the crash still spends the license it carried.
    Quarantine q;
    auto t0 = Clock::now();
    q.on_crash(compile, "d1", "cause", t0);
    q.on_change(t0);
    {
        Quarantine::Attempt attempt(q, compile);
        q.on_save();
        q.on_crash(compile, "d2", "cause", t0 + seconds(1));
    }
    EXPECT(q.barred(compile, t0 + seconds(10)));
}

ZEST_CASE(SiblingCrashOvertakes) {
    Quarantine q;
    auto t0 = Clock::now();
    Quarantine::Attempt attempt(q, hover);
    EXPECT(!attempt.overtaken());
    q.on_crash(compile, "d1", "cause", t0);
    EXPECT(!attempt.overtaken());
    q.on_crash(hover, "d2", "cause", t0);
    EXPECT(attempt.overtaken());
}

ZEST_CASE(ChangeDuringFlightStands) {
    // The crash describes the inputs the attempt carried; an edit during
    // its flight is still untried.
    Quarantine q;
    auto t0 = Clock::now();
    {
        Quarantine::Attempt attempt(q, compile);
        q.on_change(t0);
        q.on_crash(compile, "d1", "cause", t0 + seconds(20));
    }
    EXPECT(!q.barred(compile, t0 + seconds(30)));
}

ZEST_CASE(EditingCrashStaysSilent) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_change(t0);
    q.on_crash(compile, "d1", "cause", t0 + seconds(1));
    EXPECT(!q.shows(compile));
    EXPECT(q.notes().empty());

    // A repeat shows.
    q.on_change(t0 + seconds(2));
    q.on_crash(compile, "d2", "cause", t0 + seconds(5));
    EXPECT(q.shows(compile));
    EXPECT(q.notes().size() == 1u);
}

ZEST_CASE(SavedCrashShows) {
    // Saved code is no half-typed code, however recent the edit.
    Quarantine q;
    auto t0 = Clock::now();
    q.on_change(t0);
    q.on_save();
    q.on_crash(compile, "d1", "cause", t0 + seconds(1));
    EXPECT(q.shows(compile));
}

ZEST_CASE(ColdCrashShows) {
    Quarantine q;
    auto t0 = Clock::now();
    q.on_change(t0);
    q.on_crash(hover, "d1", "killed by signal 6 (SIGABRT)", t0 + Quarantine::editing_window);
    ASSERT(q.notes().size() == 1u);
    EXPECT(q.notes()[0].kind == hover);
    EXPECT(q.notes()[0].cause == "killed by signal 6 (SIGABRT)");
    EXPECT(!q.notes()[0].save_only);
}

ZEST_CASE(BudgetBlocksAtThreshold) {
    BlameBudget budget;
    EXPECT(!budget.blocked("key-a"));

    budget.on_blame("key-a");
    EXPECT(!budget.blocked("key-a"));
    budget.on_blame("key-a");
    EXPECT(budget.blocked("key-a"));

    // Keys are independent: fresh content (fresh key) starts fresh.
    EXPECT(!budget.blocked("key-b"));
}

ZEST_CASE(BudgetClearsOnLand) {
    // A consumer landing proves the blames were transient: without the
    // clear, two unrelated hiccups far apart would block a key that
    // serves fine in between.
    BlameBudget budget;
    budget.on_blame("key");
    budget.on_land("key");
    budget.on_blame("key");
    EXPECT(!budget.blocked("key"));
}

ZEST_CASE(BudgetRearmsAfterCooldown) {
    // The poison may live in content the key cannot see (a header included
    // by the hashed preamble text): a block is a cooldown, not a verdict.
    // Zero cooldown models "elapsed" — the key earns a fresh budget.
    BlameBudget budget{std::chrono::milliseconds(0)};
    budget.on_blame("key");
    budget.on_blame("key");
    EXPECT(!budget.blocked("key"));
}

};  // ZEST_SUITE(QuarantineMachine)

}  // namespace

}  // namespace clice::testing
