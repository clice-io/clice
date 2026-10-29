module clice;

import :support.failure_record;
import :unittests.utils;

namespace clice::testing {
namespace {

using Clock = FailureRecord::Clock;
using std::chrono::minutes;
using std::chrono::seconds;

ZEST_SUITE(FailureRecord) {

ZEST_CASE(StopsAtLimit) {
    FailurePolicy policy{.limit = 2};
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail(policy, {}, "cause", t0);
    ZEXPECT(!record.stops(policy, t0));
    record.fail(policy, {}, "cause", t0);
    ZEXPECT(record.stops(policy, t0));
    ZEXPECT(record.count == 2u);
}

ZEST_CASE(DeathCountsOnce) {
    FailureRecord record;
    auto t0 = Clock::now();
    ZEXPECT(record.fail({}, "sf:0:1", "cause", t0));
    ZEXPECT(!record.fail({}, "sf:0:1", "cause", t0));
    ZEXPECT(record.count == 1u);

    // A failure no worker died of always counts.
    ZEXPECT(record.fail({}, {}, "cause", t0));
    ZEXPECT(record.fail({}, {}, "cause", t0));
    ZEXPECT(record.count == 3u);
}

ZEST_CASE(CooldownStartsAfresh) {
    // The cause may lie where nothing the record sees can show a change: a
    // stop is a cooldown, not a decision, and past it the count restarts.
    FailurePolicy policy{.limit = 2, .cooldown = minutes(5)};
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail(policy, {}, "cause", t0);
    record.fail(policy, {}, "cause", t0);
    ZEXPECT(record.stops(policy, t0 + minutes(4)));
    ZEXPECT(!record.stops(policy, t0 + minutes(5)));

    record.fail(policy, {}, "cause", t0 + minutes(5));
    ZEXPECT(record.count == 1u);
    ZEXPECT(!record.stops(policy, t0 + minutes(5)));
}

ZEST_CASE(NoCooldownStays) {
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail({}, {}, "cause", t0);
    ZEXPECT(record.stops({}, t0 + minutes(60 * 24)));
}

ZEST_CASE(ChangeRetriesAfterSpacing) {
    FailurePolicy policy{.change_limit = 3, .spacing = seconds(2)};
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail(policy, {}, "cause", t0);
    record.changed = true;
    ZEXPECT(record.stops(policy, t0 + seconds(1)));
    ZEXPECT(!record.stops(policy, t0 + seconds(2)));
}

ZEST_CASE(ChangeLimitLeavesSave) {
    FailurePolicy policy{.change_limit = 3, .spacing = seconds(2)};
    FailureRecord record;
    auto t = Clock::now();
    for(int crash = 0; crash < 3; crash += 1) {
        record.fail(policy, {}, "cause", t);
        record.changed = true;
        t += seconds(10);
    }
    ZEXPECT(record.save_only(policy));
    ZEXPECT(record.stops(policy, t));

    record.save();
    ZEXPECT(!record.save_only(policy));
    ZEXPECT(!record.stops(policy, t));
}

ZEST_CASE(TryingStopsOthers) {
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail({}, {}, "cause", t0);
    record.save();
    record.trying = true;
    ZEXPECT(record.stops({}, t0));

    // A failure ends the attempt, and the save it held is spent.
    record.fail({}, {}, "cause", t0);
    ZEXPECT(!record.trying);
    ZEXPECT(record.stops({}, t0));
}

ZEST_CASE(EditingFailureHides) {
    FailureRecord record;
    auto t0 = Clock::now();
    record.fail({}, {}, "cause", t0, /*editing=*/true);
    ZEXPECT(!record.shown);
    // A repeat shows.
    record.fail({}, {}, "cause", t0, /*editing=*/true);
    ZEXPECT(record.shown);
}

};  // ZEST_SUITE(FailureRecord)

}  // namespace
}  // namespace clice::testing
