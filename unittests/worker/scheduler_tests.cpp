module clice;

import :unittests.utils;
import :worker.scheduler;

namespace clice::testing {

namespace {

using chrono::milliseconds;
using chrono::minutes;
using chrono::seconds;
using worker::Priority;

constexpr std::uint64_t GiB = 1ull << 30;
constexpr std::uint64_t MiB = 1ull << 20;

/// A scheduler with `workers` stateless slots in service, sampled once,
/// and the actions it took collected in order.
struct Core {
    WorkerScheduler scheduler;
    SchedulerTime now{};
    std::vector<SchedulerAction> actions;
    std::size_t processes = 0;

    explicit Core(std::size_t workers,
                  SchedulerOptions options = {.background_jobs = 4, .max_workers = 6}) :
        scheduler(options), processes(workers) {
        for(std::size_t slot = 0; slot < workers; slot += 1) {
            scheduler.worker_ready(slot, now);
        }
        sample(16 * GiB);
    }

    void take() {
        ranges::move(scheduler.take_actions(), std::back_inserter(actions));
    }

    void advance(chrono::milliseconds by) {
        now += by;
    }

    void sample(std::uint64_t headroom,
                std::vector<SchedulerSample::Worker> workers = {},
                std::uint64_t reserve = 512 * MiB) {
        scheduler.sample({.headroom = headroom,
                          .reserve = reserve,
                          .stateless_processes = processes,
                          .workers = std::move(workers)},
                         now);
        take();
    }

    void submit(RequestId id, Priority priority, RequestCost cost = {}, bool build = true) {
        scheduler.submit(id, priority, cost, build, now);
        take();
    }

    void finish(RequestId id) {
        scheduler.finish(id, now);
        take();
    }

    /// The slot a Dispatch put the request on.
    std::optional<std::size_t> slot_of(RequestId id) const {
        for(auto& action: actions) {
            if(auto* dispatch = std::get_if<scheduler::Dispatch>(&action);
               dispatch && dispatch->request == id) {
                return dispatch->slot;
            }
        }
        return std::nullopt;
    }

    template <typename Action>
    std::vector<Action> all() const {
        std::vector<Action> found;
        for(auto& action: actions) {
            if(auto* one = std::get_if<Action>(&action)) {
                found.push_back(*one);
            }
        }
        return found;
    }
};

ZEST_SUITE(WorkerScheduler) {

ZEST_CASE(ForegroundTakesIdleWorker) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::High);
    ZEXPECT(core.slot_of(1) == std::optional<std::size_t>(0));
    ZEXPECT(core.slot_of(2) == std::optional<std::size_t>(1));
    ZEXPECT(core.all<scheduler::Cancel>().empty());
}

ZEST_CASE(ForegroundStopsNewestBackground) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.advance(seconds(20));
    core.submit(2, Priority::Low);
    core.advance(seconds(20));
    core.submit(3, Priority::High);
    // The user is active now: one background request at most, and the
    // foreground one needs a worker; the newest yields.
    auto cancels = core.all<scheduler::Cancel>();
    ZASSERT(cancels.size() == 1u);
    ZEXPECT(cancels[0].request == 2u);
    ZEXPECT(!core.slot_of(3));
    core.finish(2);
    ZEXPECT(core.slot_of(3) == std::optional<std::size_t>(1));
}

ZEST_CASE(ActiveUserOneJob) {
    Core core(4);
    core.scheduler.user_activity(core.now);
    core.take();
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    ZEXPECT(core.slot_of(1).has_value());
    ZEXPECT(!core.slot_of(2));
    core.advance(WorkerScheduler::foreground_hold);
    core.sample(16 * GiB);
    ZEXPECT(core.slot_of(2).has_value());
}

ZEST_CASE(StuckForegroundReleases) {
    Core core(4);
    core.submit(1, Priority::High);
    core.submit(2, Priority::Low);
    core.submit(3, Priority::Low);
    ZEXPECT(!core.slot_of(3));
    // A foreground request older than the hold no longer holds the
    // background at one.
    core.advance(WorkerScheduler::foreground_request_hold);
    core.sample(16 * GiB);
    ZEXPECT(core.slot_of(3).has_value());
}

ZEST_CASE(MemoryAdmitsByPeak) {
    Core core(4);
    core.sample(4 * GiB);
    for(RequestId id = 1; id <= 3; id += 1) {
        core.submit(id, Priority::Low);
    }
    // Two unknown units (1.5 GiB each) leave the 0.5 GiB reserve of 4 GiB;
    // a third does not.
    ZEXPECT(core.slot_of(1).has_value());
    ZEXPECT(core.slot_of(2).has_value());
    ZEXPECT(!core.slot_of(3));
    core.finish(1);
    ZEXPECT(core.slot_of(3).has_value());
}

ZEST_CASE(KnownPeakAdmitsMore) {
    Core core(4);
    core.sample(4 * GiB);
    for(RequestId id = 1; id <= 4; id += 1) {
        core.submit(id, Priority::Low, {.peak = 800 * MiB});
    }
    ZEXPECT(core.slot_of(4).has_value());
}

ZEST_CASE(GrowthStillDueCounts) {
    Core core(4);
    core.submit(1, Priority::Low, {.peak = 3 * GiB});
    // The first unit has grown to 1 GiB of its 3: 2 GiB are still due.
    core.sample(3 * GiB,
                {
                    {.slot = 0, .memory = 1 * GiB}
    });
    core.submit(2, Priority::Low, {.peak = 1 * GiB});
    ZEXPECT(!core.slot_of(2));
    core.sample(4 * GiB,
                {
                    {.slot = 0, .memory = 1 * GiB}
    });
    ZEXPECT(core.slot_of(2).has_value());
}

ZEST_CASE(KillsNewestInTurn) {
    Core core(4);
    core.submit(1, Priority::Low);
    core.advance(seconds(1));
    core.submit(2, Priority::Low);
    core.advance(seconds(1));
    core.submit(3, Priority::Low);
    core.sample(256 * MiB,
                {
                    {.slot = 0, .memory = 3 * GiB},
                    {.slot = 2, .memory = 1 * GiB}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].requests.front() == 3u);
    ZEXPECT(kills[0].reason == scheduler::KillReason::Memory);
    ZEXPECT(!core.scheduler.runs_alone(kills[0].slot));
    // Still short, and the killed process holds its memory until it exits,
    // though it left service at once.
    core.scheduler.worker_gone(2, false, core.now);
    core.advance(seconds(1));
    core.sample(256 * MiB);
    ZEXPECT(core.all<scheduler::Kill>().size() == 1u);
    core.scheduler.worker_exited(2);
    core.sample(256 * MiB);
    kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 2u);
    ZEXPECT(kills[1].requests.front() == 2u);
}

ZEST_CASE(LoneKillTooBig) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(core.scheduler.runs_alone(kills[0].slot));
}

ZEST_CASE(LoneUnitGetsIdleMemory) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    ZEXPECT(core.all<scheduler::ScaleDown>().size() == 1u);
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    ZEXPECT(core.all<scheduler::Kill>().size() == 1u);
}

ZEST_CASE(KilledMemoryJoinsPeaks) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.sample(256 * MiB,
                {
                    {.slot = 1, .memory = 6 * GiB}
    });
    ZASSERT(core.all<scheduler::Kill>().size() == 1u);
    ZEXPECT(core.scheduler.unknown_peak() == 6 * GiB);
}

ZEST_CASE(LoneUnitUsesReserve) {
    Core core(1);
    core.submit(1, Priority::Low);
    // Within the reserve, above its third: the only request goes on.
    core.sample(300 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    ZEXPECT(core.all<scheduler::Kill>().size() == 1u);
}

ZEST_CASE(SwappingStopsNewest) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.scheduler.sample(
        {.headroom = 8 * GiB, .reserve = 512 * MiB, .swapping = true, .stateless_processes = 2},
        core.now);
    core.take();
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].requests.front() == 2u);
}

ZEST_CASE(LoneUnitSwapsOn) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.scheduler.sample(
        {.headroom = 8 * GiB, .reserve = 512 * MiB, .swapping = true, .stateless_processes = 1},
        core.now);
    core.take();
    ZEXPECT(core.all<scheduler::Kill>().empty());
}

ZEST_CASE(ForegroundBesideNotLone) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::High);
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].requests.front() == 1u);
    ZEXPECT(!core.scheduler.runs_alone(kills[0].slot));
}

ZEST_CASE(DocumentCompileNotLone) {
    Core core(1);
    core.submit(1, Priority::Low);
    // An open document's compile takes the memory: the unit is not alone.
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.take();
    core.sample(300 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(!core.scheduler.runs_alone(kills[0].slot));
}

ZEST_CASE(ForegroundNeverKilled) {
    Core core(2);
    core.submit(1, Priority::High);
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    ZEXPECT(core.all<scheduler::Kill>().empty());
}

ZEST_CASE(IdleQueueStartsHead) {
    // Nothing else runs: waiting frees no memory, and a kill alone then
    // shows the unit does not fit.
    Core core(2);
    core.sample(2 * GiB);
    core.submit(1, Priority::Low, {.peak = 6 * GiB});
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(SmallerUnitsGoFirst) {
    Core core(3);
    core.submit(1, Priority::Low, {.peak = 1 * GiB});
    core.sample(3 * GiB,
                {
                    {.slot = 0, .memory = 1 * GiB}
    });
    core.submit(2, Priority::Low, {.peak = 6 * GiB});
    core.submit(3, Priority::Low, {.peak = 1 * GiB});
    ZEXPECT(!core.slot_of(2));
    ZEXPECT(core.slot_of(3).has_value());
    // Past long_wait the queue waits for its head.
    core.advance(WorkerScheduler::long_wait);
    core.submit(4, Priority::Low, {.peak = 1 * GiB});
    core.finish(3);
    ZEXPECT(!core.slot_of(4));
    core.finish(1);
    ZEXPECT(core.slot_of(2).has_value());
}

ZEST_CASE(KillFloorRaisesEstimate) {
    Core core(3);
    core.sample(4 * GiB);
    core.submit(1, Priority::Low, {.peak = 1 * GiB});
    // Unknown, at least 1.5 GiB, fits beside it; one that reached 3 GiB
    // when it was last killed does not.
    core.submit(2, Priority::Low, {.floor = 3 * GiB});
    ZEXPECT(!core.slot_of(2));
    core.submit(3, Priority::Low);
    ZEXPECT(core.slot_of(3).has_value());
}

ZEST_CASE(LoneGrowthBoundByEstimate) {
    // Fast growth projects past the reserve's third, but the estimate
    // leaves it 256 MiB more: the unit fits.
    Core core(1);
    core.submit(1, Priority::Low);
    core.sample(2560 * MiB,
                {
                    {.slot = 0, .memory = 256 * MiB}
    });
    core.advance(seconds(1));
    core.sample(1536 * MiB,
                {
                    {.slot = 0, .memory = 1280 * MiB}
    });
    ZEXPECT(core.all<scheduler::Kill>().empty());
}

ZEST_CASE(PeakNotInherited) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    core.finish(1);
    // The slot's last sample is the previous unit's.
    core.submit(2, Priority::Low);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = 300 * MiB}
    });
    ZEXPECT(core.scheduler.finish(2, core.now) == 300 * MiB);
}

ZEST_CASE(ForegroundGetsFreedSlot) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.submit(3, Priority::High);
    core.finish(1);
    ZEXPECT(core.slot_of(3) == std::optional<std::size_t>(0));
    ZEXPECT(!core.slot_of(2));
}

ZEST_CASE(IdleUserJobsCap) {
    Core core(4, {.background_jobs = 2, .max_workers = 6});
    for(RequestId id = 1; id <= 3; id += 1) {
        core.submit(id, Priority::Low);
    }
    ZEXPECT(core.slot_of(2).has_value());
    ZEXPECT(!core.slot_of(3));
}

ZEST_CASE(ScalingDownTakesNothing) {
    Core core(3);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB);
    ZASSERT(core.all<scheduler::ScaleDown>().size() == 1u);
    for(RequestId id = 1; id <= 3; id += 1) {
        core.submit(id, Priority::Low);
    }
    ZEXPECT(core.slot_of(2).has_value());
    ZEXPECT(!core.slot_of(3));
}

ZEST_CASE(AskedCancelCounts) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.submit(3, Priority::High);
    ZEXPECT(core.all<scheduler::Cancel>().size() == 1u);
    // The cancel asked still frees the worker the foreground waits for.
    core.sample(16 * GiB);
    core.scheduler.user_activity(core.now);
    core.take();
    ZEXPECT(core.all<scheduler::Cancel>().size() == 1u);
}

ZEST_CASE(QueuedBackgroundSpawns) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    ZEXPECT(core.all<scheduler::Spawn>().empty());
    core.sample(16 * GiB);
    ZASSERT(core.all<scheduler::Spawn>().size() == 1u);
    core.scheduler.worker_ready(1, core.now);
    core.take();
    ZEXPECT(core.slot_of(2) == std::optional<std::size_t>(1));
}

ZEST_CASE(StarvedMachineHoldsAll) {
    // Below where a request alone would be killed, nothing starts.
    Core core(2);
    core.sample(128 * MiB);
    core.submit(1, Priority::Low, {.peak = 100 * MiB});
    core.advance(minutes(10));
    core.sample(128 * MiB);
    ZEXPECT(!core.slot_of(1));
    core.sample(2 * GiB);
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(LoneStartsInsideReserve) {
    // A limit the master takes half of leaves the headroom under the reserve
    // for good: a request alone still starts, given room to grow above the
    // line where it is found too big.
    Core core(1);
    core.sample(300 * MiB);
    core.submit(1, Priority::Low);
    ZEXPECT(!core.slot_of(1));
    core.sample(504 * MiB);
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(CompanyKeepsReserve) {
    // Beside an open document's compile a lone request is killed below the
    // reserve, so it does not start there.
    Core core(1);
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.take();
    core.sample(504 * MiB);
    core.submit(1, Priority::Low);
    ZEXPECT(!core.slot_of(1));
    core.scheduler.stateful_end(10, core.now);
    core.take();
    core.sample(504 * MiB);
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(RaisedRequestJumpsQueue) {
    // The user comes to wait for a queued background request: it runs as
    // foreground work, ahead of the background queue and of the cap an
    // active user puts on it.
    Core core(1);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.submit(3, Priority::Low);
    ZEXPECT(core.slot_of(1).has_value());
    core.scheduler.raise(3, core.now);
    core.take();
    core.finish(1);
    ZEXPECT(core.slot_of(3).has_value());
    ZEXPECT(!core.slot_of(2));
}

ZEST_CASE(RaisedRequestKeepsWorker) {
    // A running background request the user came to wait for is no longer
    // the one a foreground request stops.
    Core core(1);
    core.submit(1, Priority::Low);
    core.scheduler.raise(1, core.now);
    core.take();
    core.submit(2, Priority::High);
    ZEXPECT(core.all<scheduler::Cancel>().empty());
}

ZEST_CASE(ReportedPeakCounts) {
    // A request shorter than the sample interval reports its peak itself.
    Core core(1);
    core.submit(1, Priority::Low);
    ZASSERT(core.slot_of(1).has_value());
    core.scheduler.reported_peak(0, 700 * MiB);
    ZEXPECT(core.scheduler.finish(1, core.now) == 700 * MiB);
}

ZEST_CASE(BuildDeadlineIsCpu) {
    Core core(1);
    core.submit(1, Priority::Low, {.peak = 200 * MiB});
    // Twenty minutes waiting for the CPU spend two of it: no kill.
    for(int minute = 0; minute <= 20; minute += 1) {
        core.sample(16 * GiB,
                    {
                        {.slot = 0, .cpu = milliseconds(minutes(minute)) / 10}
        });
        core.advance(minutes(1));
    }
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.sample(16 * GiB,
                {
                    {.slot = 0, .cpu = minutes(11)}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].reason == scheduler::KillReason::Deadline);
}

ZEST_CASE(StalledCpuIsHung) {
    Core core(1);
    core.submit(1, Priority::Low, {.peak = 200 * MiB});
    core.sample(16 * GiB,
                {
                    {.slot = 0, .cpu = seconds(3)}
    });
    core.advance(minutes(5) + seconds(1));
    core.sample(16 * GiB,
                {
                    {.slot = 0, .cpu = seconds(3)}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].reason == scheduler::KillReason::Stalled);
}

ZEST_CASE(UnstartedRequestUntimed) {
    Core core(1);
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.scheduler.stateful_started(10, core.now);
    // The query waits for its worker to begin it, behind the build.
    core.scheduler.stateful_begin(11, 0, false, core.now);
    core.advance(minutes(3));
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.advance(minutes(8));
    core.sample(16 * GiB);
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].stateful);
    ZEXPECT(kills[0].requests == std::vector<RequestId>{10});
}

ZEST_CASE(BuildTimedFromStart) {
    // A build that waited for a thread of its worker behind other builds
    // is timed from when the worker began it, not from its send.
    Core core(1);
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.scheduler.stateful_started(10, core.now);
    core.scheduler.stateful_begin(11, 0, true, core.now);
    core.advance(minutes(9));
    core.scheduler.stateful_end(10, core.now);
    core.scheduler.stateful_started(11, core.now);
    core.advance(minutes(2));
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.advance(minutes(9));
    core.sample(16 * GiB);
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].requests == std::vector<RequestId>{11});
}

ZEST_CASE(StartedQueryTimed) {
    // A query begun is timed by its own deadline, whatever builds its
    // worker runs beside it.
    Core core(1);
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.scheduler.stateful_started(10, core.now);
    core.scheduler.stateful_begin(11, 0, false, core.now);
    core.scheduler.stateful_started(11, core.now);
    core.advance(seconds(90));
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.advance(minutes(1));
    core.sample(16 * GiB);
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].requests == std::vector<RequestId>{11});
}

ZEST_CASE(IgnoredCancelIsKilled) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.scheduler.cancel(1, core.now);
    core.take();
    ZEXPECT(core.all<scheduler::Cancel>().size() == 1u);
    core.advance(WorkerScheduler::cancel_grace + seconds(1));
    core.sample(16 * GiB);
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].reason == scheduler::KillReason::CancelGrace);
}

ZEST_CASE(QueuedCancelDrops) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.scheduler.cancel(2, core.now);
    core.finish(1);
    ZEXPECT(!core.slot_of(2));
}

ZEST_CASE(IdlePoolScalesDown) {
    Core core(3);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB);
    auto scale_downs = core.all<scheduler::ScaleDown>();
    ZASSERT(scale_downs.size() == 1u);
    ZEXPECT(scale_downs[0].slot == 2u);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB);
    // Down to min_workers, one.
    ZEXPECT(core.all<scheduler::ScaleDown>().size() == 2u);
}

ZEST_CASE(HeavyIdleWorkerReplaced) {
    constexpr auto heavy = WorkerScheduler::replace_above + MiB;
    Core core(1);
    core.submit(1, Priority::Low);
    core.finish(1);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = heavy}
    });
    // A fresh process first: the heavy one serves until it is ready.
    ZEXPECT(core.all<scheduler::Spawn>().size() == 1u);
    ZEXPECT(core.all<scheduler::ScaleDown>().empty());
    core.scheduler.worker_ready(1, core.now);
    core.processes = 2;
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = heavy  },
                    {.slot = 1, .memory = 3 * MiB}
    });
    auto scale_downs = core.all<scheduler::ScaleDown>();
    ZASSERT(scale_downs.size() == 1u);
    ZEXPECT(scale_downs[0].slot == 0u);
}

ZEST_CASE(LightIdleWorkerKept) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.finish(1);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = WorkerScheduler::replace_above}
    });
    ZEXPECT(core.all<scheduler::Spawn>().empty());
}

ZEST_CASE(UnusedWorkerKept) {
    Core core(1);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = 2 * WorkerScheduler::replace_above}
    });
    ZEXPECT(core.all<scheduler::Spawn>().empty());
}

ZEST_CASE(ScaleDownTakesHeaviest) {
    Core core(3);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = MiB    },
                    {.slot = 1, .memory = 9 * MiB},
                    {.slot = 2, .memory = MiB    }
    });
    auto scale_downs = core.all<scheduler::ScaleDown>();
    ZASSERT(scale_downs.size() == 1u);
    ZEXPECT(scale_downs[0].slot == 1u);
}

ZEST_CASE(ShortMemoryTakesHeavyIdle) {
    constexpr auto heavy = WorkerScheduler::replace_above + MiB;
    Core core(1);
    core.submit(1, Priority::Low);
    core.finish(1);
    core.advance(WorkerScheduler::idle_scale_down);
    core.sample(16 * GiB,
                {
                    {.slot = 0, .memory = heavy}
    });
    ZASSERT(core.all<scheduler::Spawn>().size() == 1u);
    core.scheduler.worker_ready(1, core.now);
    core.processes = 2;
    // Short of memory before the idle time is up: the heavy one goes, not
    // its fresh replacement.
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = heavy  },
                    {.slot = 1, .memory = 3 * MiB}
    });
    auto scale_downs = core.all<scheduler::ScaleDown>();
    ZASSERT(scale_downs.size() == 1u);
    ZEXPECT(scale_downs[0].slot == 0u);
}

ZEST_CASE(StarvedForegroundSpawns) {
    Core core(1);
    core.submit(1, Priority::High);
    core.submit(2, Priority::High);
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Spawn>().empty());
    core.advance(WorkerScheduler::starved_spawn);
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Spawn>().size() == 1u);
}

ZEST_CASE(PeaksLearnedPercentile) {
    Core core(1);
    ZEXPECT(core.scheduler.unknown_peak() == WorkerScheduler::default_peak);
    for(RequestId id = 1; id <= 4; id += 1) {
        core.submit(id, Priority::Low, {.peak = 100 * MiB});
        core.sample(16 * GiB,
                    {
                        {.slot = 0, .memory = id * GiB}
        });
        core.finish(id);
    }
    ZEXPECT(core.scheduler.unknown_peak() == 4 * GiB);
}

ZEST_CASE(SmallPeaksDropFloor) {
    Core core(1);
    for(RequestId id = 1; id <= WorkerScheduler::trusted_peaks; id += 1) {
        ZEXPECT(core.scheduler.unknown_peak() == WorkerScheduler::default_peak);
        core.submit(id, Priority::Low);
        core.sample(16 * GiB,
                    {
                        {.slot = 0, .memory = 200 * MiB}
        });
        core.finish(id);
    }
    ZEXPECT(core.scheduler.unknown_peak() == 200 * MiB);
}

ZEST_CASE(LearnedEstimateRunsCapped) {
    Core core(5, {.background_jobs = 5, .max_workers = 5});
    for(RequestId id = 1; id <= WorkerScheduler::trusted_peaks; id += 1) {
        core.submit(id, Priority::Low);
        core.sample(16 * GiB,
                    {
                        {.slot = 0, .memory = 200 * MiB}
        });
        core.finish(id);
    }
    core.sample(2 * GiB);
    core.actions.clear();
    for(RequestId id = 10; id < 15; id += 1) {
        core.submit(id, Priority::Low);
    }
    // Three run on 200 MiB each; the fourth, at 1.5 GiB, does not fit
    // beside them in 2 GiB.
    ZEXPECT(core.all<scheduler::Dispatch>().size() == WorkerScheduler::learned_runs);
}

};  // ZEST_SUITE(WorkerScheduler)

}  // namespace

}  // namespace clice::testing
