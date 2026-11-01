module clice;

import :project.indexer;
import :unittests.utils;

namespace clice::testing {
namespace {

using Decision = ReindexQueue::FailureDecision;
using Failure = ReindexQueue::Failure;

/// The take/finish contract and the order in isolation: a pending
/// reindex is taken atomically at dispatch, flight-time recordings book a
/// newer one, success consumes only the taken one, failure merges it back
/// — bounded for crashes — and a pass runs over the files queued when it
/// began.
ZEST_SUITE(ReindexQueue) {

constexpr static unsigned budget = 3;

clice::ReindexQueue queue{budget};

/// Take the next place as a dispatch does, in a pass of its own.
std::optional<clice::ReindexQueue::TakenReindex> take() {
    queue.begin_pass([](Fid) { return false; });
    auto taken = queue.take_next();
    queue.end_pass();
    return taken;
}

/// The files of the places not taken yet, in their order, taking them.
std::vector<Fid> drain() {
    std::vector<Fid> order;
    queue.begin_pass([](Fid) { return false; });
    while(auto taken = queue.take_next()) {
        order.push_back(taken->id);
        queue.finish(*taken);
    }
    queue.end_pass();
    return order;
}

ZEST_CASE(content_absorbs_queued) {
    // Within one queued place ContentChanged absorbs: a deps-only cascade
    // cannot downgrade a file whose own content already changed, and no
    // second place is owed.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(queue.queued() == 1u);
    ZEXPECT(queue.pending_reason(Fid{1}) == ReindexReason::ContentChanged);

    queue.record(Fid{1}, ReindexReason::ContentChanged);
    ZEXPECT(queue.queued() == 1u);
    ZEXPECT(queue.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
}

ZEST_CASE(consumed_pass_owns_reindex) {
    // A deps-only recording after the place was taken is a new reindex of its
    // own kind: the in-flight pass owns the earlier content change, and
    // keeping ContentChanged would suppress the file's index past it.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = take();
    ZASSERT(taken);
    ZEXPECT(queue.in_flight(Fid{1}));

    queue.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(queue.queued() == 1u);
    ZEXPECT(!queue.in_flight(Fid{1}));
    ZEXPECT(queue.pending_reason(Fid{1}) == ReindexReason::DepsOnly);
}

ZEST_CASE(finish_spares_newer_reindex) {
    // A recording during the flight books a newer reindex; the older attempt's
    // finish must leave it standing, and only the newer taken one clears it.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto old_taken = take();
    ZASSERT(old_taken);
    queue.record(Fid{1}, ReindexReason::ContentChanged);

    queue.finish(*old_taken);
    ZEXPECT(queue.pending_reason(Fid{1}));

    auto fresh = take();
    ZASSERT(fresh);
    queue.finish(*fresh);
    ZEXPECT(!queue.contains(Fid{1}));
    ZEXPECT(queue.queued() == 0u);
}

ZEST_CASE(deps_never_outdate) {
    // Only newer content makes an in-flight pass outdated: its index describes
    // text that no longer exists. A deps-only recording does not — the
    // index is positionally right and the follow-up covers the drift.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = take();
    ZASSERT(taken);
    ZEXPECT(!queue.outdated(*taken));

    queue.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(!queue.outdated(*taken));

    queue.record(Fid{1}, ReindexReason::ContentChanged);
    ZEXPECT(queue.outdated(*taken));

    // A cleared entry (file removed) also commits nothing.
    queue.clear(Fid{1});
    ZEXPECT(queue.outdated(*taken));
}

ZEST_CASE(outdated_failure_spends_nothing) {
    // A failed dispatch of outdated bytes neither requeues nor spends
    // the fresh content's crash budget: the newer booking redoes the
    // work with a full budget of its own.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto stale = take();
    ZASSERT(stale);
    queue.record(Fid{1}, ReindexReason::ContentChanged);

    ZEXPECT(queue.on_dispatch_failure(*stale, Failure::Lost) == Decision::Outdated);
    ZEXPECT(queue.queued() == 1u);

    // The fresh reindex still has the whole budget: `budget` crashes
    // requeue before the next one abandons.
    for(unsigned i = 0; i < budget; i += 1) {
        auto taken = take();
        ZASSERT(taken);
        ZEXPECT(queue.on_dispatch_failure(*taken, Failure::Lost) == Decision::Requeued);
        ZEXPECT(queue.queued() == 1u);
    }
    auto last = take();
    ZASSERT(last);
    ZEXPECT(queue.on_dispatch_failure(*last, Failure::Lost) == Decision::GaveUp);
    ZEXPECT(!queue.contains(Fid{1}));
    ZEXPECT(queue.queued() == 0u);
}

ZEST_CASE(preemption_needs_no_budget) {
    // Preemptions say nothing about the file: they requeue past any spent
    // budget — capping them would silently drop coverage.
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    for(int i = 0; i < 10; i += 1) {
        auto taken = take();
        ZASSERT(taken);
        ZEXPECT(queue.on_dispatch_failure(*taken, Failure::Preempted) == Decision::Requeued);
    }
}

ZEST_CASE(requeue_carries_content) {
    // The requeue carries the reason the dispatch was launched for: a
    // deps-only downgrade during the flight bet on the content pass
    // committing, and a failed pass leaves the edit uncovered.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = take();
    ZASSERT(taken);
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    ZEXPECT(queue.pending_reason(Fid{1}) == ReindexReason::DepsOnly);

    ZEXPECT(queue.on_dispatch_failure(*taken, Failure::Lost) == Decision::Requeued);
    ZEXPECT(queue.pending_reason(Fid{1}) == ReindexReason::ContentChanged);
    ZEXPECT(queue.queued() == 1u);
}

ZEST_CASE(own_crash_gives_up) {
    // A run that crashed its worker would crash the retry too: no requeue,
    // the file waits for its content to change.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = take();
    ZASSERT(taken);
    ZEXPECT(queue.on_dispatch_failure(*taken, Failure::Crashed) == Decision::GaveUp);
    ZEXPECT(!queue.contains(Fid{1}));
}

ZEST_CASE(cleared_entry_drops) {
    // A file removed mid-flight has nothing to redo.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    auto taken = take();
    ZASSERT(taken);
    queue.clear(Fid{1});
    ZEXPECT(queue.on_dispatch_failure(*taken, Failure::Lost) == Decision::Dropped);
    ZEXPECT(!queue.contains(Fid{1}));
}

ZEST_CASE(cleared_place_reused) {
    // A file removed from disk and recreated before its place was consumed
    // owes no second place: both would dispatch it in one pass.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    queue.clear(Fid{1});
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    ZEXPECT(queue.queued() == 1u);
    ZEXPECT(take().has_value());
    ZEXPECT(queue.queued() == 0u);
}

ZEST_CASE(cleared_place_skipped) {
    // A place whose file was cleared before its turn dispatches nothing.
    queue.record(Fid{1}, ReindexReason::ContentChanged);
    queue.record(Fid{2}, ReindexReason::ContentChanged);
    queue.clear(Fid{1});
    ZEXPECT(drain() == std::vector<Fid>{Fid{2}});
    ZEXPECT(queue.queued() == 0u);
}

ZEST_CASE(pass_takes_its_places) {
    // A pass runs over the files queued when it began, those `first` picks
    // ahead: one queued during the pass, a requeue of its own failures
    // among them, waits for the next.
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    queue.record(Fid{2}, ReindexReason::DepsOnly);
    queue.record(Fid{3}, ReindexReason::DepsOnly);
    ZEXPECT(queue.begin_pass([](Fid id) { return id == Fid{3}; }) == 3u);

    auto first = queue.take_next();
    ZASSERT(first);
    ZEXPECT(first->id == Fid{3});
    ZEXPECT(queue.on_dispatch_failure(*first, Failure::Preempted) == Decision::Requeued);
    queue.record(Fid{4}, ReindexReason::DepsOnly);

    std::vector<Fid> order;
    while(auto taken = queue.take_next()) {
        order.push_back(taken->id);
        queue.finish(*taken);
    }
    queue.end_pass();
    ZEXPECT(order == std::vector<Fid>{Fid{1}, Fid{2}});
    ZEXPECT(queue.queued() == 2u);
    ZEXPECT(drain() == std::vector<Fid>{Fid{3}, Fid{4}});
}

ZEST_CASE(move_to_front_within_pass) {
    // A file someone reads leads the places the running pass has left.
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    queue.record(Fid{2}, ReindexReason::DepsOnly);
    queue.record(Fid{3}, ReindexReason::DepsOnly);
    queue.begin_pass([](Fid) { return false; });
    auto taken = queue.take_next();
    ZASSERT(taken);
    queue.finish(*taken);

    queue.move_to_front(Fid{3});
    std::vector<Fid> order;
    while(auto next = queue.take_next()) {
        order.push_back(next->id);
        queue.finish(*next);
    }
    queue.end_pass();
    ZEXPECT(order == std::vector<Fid>{Fid{3}, Fid{2}});
}

ZEST_CASE(move_to_front_spares_pass) {
    // A place queued past the running pass leads the next one instead of
    // joining this one: the pass may have the file in flight.
    queue.record(Fid{1}, ReindexReason::DepsOnly);
    queue.record(Fid{2}, ReindexReason::DepsOnly);
    queue.begin_pass([](Fid) { return false; });
    auto taken = queue.take_next();
    ZASSERT(taken);
    queue.record(Fid{3}, ReindexReason::DepsOnly);
    queue.record(Fid{1}, ReindexReason::DepsOnly);

    queue.move_to_front(Fid{1});
    queue.finish(*taken);
    auto rest = queue.take_next();
    ZASSERT(rest);
    ZEXPECT(rest->id == Fid{2});
    queue.finish(*rest);
    ZEXPECT(!queue.take_next());
    queue.end_pass();
    ZEXPECT(drain() == std::vector<Fid>{Fid{1}, Fid{3}});
}

};  // ZEST_SUITE(ReindexQueue)

}  // namespace
}  // namespace clice::testing
