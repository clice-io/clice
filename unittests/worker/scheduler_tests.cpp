module;

#include "modules/prelude.h"

module clice;

import :unittests.utils;
import :worker.scheduler;

namespace clice::testing {

namespace {

using std::chrono::milliseconds;
using std::chrono::minutes;
using std::chrono::seconds;
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
        std::ranges::move(scheduler.take_actions(), std::back_inserter(actions));
    }

    void advance(std::chrono::milliseconds by) {
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

    void submit(RequestId id, Priority priority, std::uint64_t peak = 0, bool build = true) {
        scheduler.submit(id, priority, {.peak = peak}, build, now);
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

ZEST_CASE(ActiveUserGetsOneJob) {
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
        core.submit(id, Priority::Low, 800 * MiB);
    }
    ZEXPECT(core.slot_of(4).has_value());
}

ZEST_CASE(GrowthStillDueCounts) {
    Core core(4);
    core.submit(1, Priority::Low, 3 * GiB);
    // The first unit has grown to 1 GiB of its 3: 2 GiB are still due.
    core.sample(3 * GiB,
                {
                    {.slot = 0, .memory = 1 * GiB}
    });
    core.submit(2, Priority::Low, 1 * GiB);
    ZEXPECT(!core.slot_of(2));
    core.sample(4 * GiB,
                {
                    {.slot = 0, .memory = 1 * GiB}
    });
    ZEXPECT(core.slot_of(2).has_value());
}

ZEST_CASE(KillsNewestOneAtATime) {
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
    ZEXPECT(kills[0].request == 3u);
    ZEXPECT(kills[0].reason == scheduler::KillReason::Memory);
    ZEXPECT(!core.scheduler.runs_alone(kills[0].slot));
    // Still short, but the first kill's process is not gone yet.
    core.advance(seconds(1));
    core.sample(256 * MiB);
    ZEXPECT(core.all<scheduler::Kill>().size() == 1u);
    core.scheduler.worker_gone(2, false, core.now);
    core.sample(256 * MiB);
    kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 2u);
    ZEXPECT(kills[1].request == 2u);
}

ZEST_CASE(AloneKillIsTooBig) {
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

ZEST_CASE(LoneUnitUsesReserve) {
    Core core(1);
    core.submit(1, Priority::Low);
    // Within the reserve, above its third: the only request goes on.
    core.sample(300 * MiB, {{.slot = 0, .memory = 6 * GiB}});
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.sample(100 * MiB, {{.slot = 0, .memory = 6 * GiB}});
    ZEXPECT(core.all<scheduler::Kill>().size() == 1u);
}

ZEST_CASE(SwappingStopsNewest) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::Low);
    core.scheduler.sample({.headroom = 8 * GiB,
                           .reserve = 512 * MiB,
                           .swapping = true,
                           .stateless_processes = 2},
                          core.now);
    core.take();
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].request == 2u);
}

ZEST_CASE(LoneUnitSwapsOn) {
    Core core(1);
    core.submit(1, Priority::Low);
    core.scheduler.sample({.headroom = 8 * GiB,
                           .reserve = 512 * MiB,
                           .swapping = true,
                           .stateless_processes = 1},
                          core.now);
    core.take();
    ZEXPECT(core.all<scheduler::Kill>().empty());
}

ZEST_CASE(ForegroundBesideIsNotAlone) {
    Core core(2);
    core.submit(1, Priority::Low);
    core.submit(2, Priority::High);
    core.sample(100 * MiB,
                {
                    {.slot = 0, .memory = 6 * GiB}
    });
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].request == 1u);
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

ZEST_CASE(HeldUnitRunsAlone) {
    Core core(2);
    core.sample(2 * GiB);
    core.submit(1, Priority::Low, 6 * GiB);
    ZEXPECT(!core.slot_of(1));
    core.advance(WorkerScheduler::long_wait);
    core.sample(2 * GiB);
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(StarvedMachineHoldsAll) {
    Core core(2);
    core.sample(256 * MiB);
    core.submit(1, Priority::Low, 200 * MiB);
    core.advance(minutes(10));
    core.sample(256 * MiB);
    ZEXPECT(!core.slot_of(1));
    core.sample(2 * GiB);
    ZEXPECT(core.slot_of(1).has_value());
}

ZEST_CASE(BuildDeadlineIsCpu) {
    Core core(1);
    core.submit(1, Priority::Low, 200 * MiB);
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
    core.submit(1, Priority::Low, 200 * MiB);
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

ZEST_CASE(QueryWaitsBehindBuild) {
    Core core(1);
    core.scheduler.stateful_begin(10, 0, true, core.now);
    core.scheduler.stateful_begin(11, 0, false, core.now);
    core.advance(minutes(3));
    core.sample(16 * GiB);
    ZEXPECT(core.all<scheduler::Kill>().empty());
    core.advance(minutes(8));
    core.sample(16 * GiB);
    auto kills = core.all<scheduler::Kill>();
    ZASSERT(kills.size() == 1u);
    ZEXPECT(kills[0].stateful);
    ZEXPECT(kills[0].request == 10u);
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

ZEST_CASE(IdlePoolRetires) {
    Core core(3);
    core.advance(WorkerScheduler::idle_retire);
    core.sample(16 * GiB);
    auto retires = core.all<scheduler::Retire>();
    ZASSERT(retires.size() == 1u);
    ZEXPECT(retires[0].slot == 2u);
    core.advance(WorkerScheduler::idle_retire);
    core.sample(16 * GiB);
    core.advance(WorkerScheduler::idle_retire);
    core.sample(16 * GiB);
    // Down to min_workers, one.
    ZEXPECT(core.all<scheduler::Retire>().size() == 2u);
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
        core.submit(id, Priority::Low, 100 * MiB);
        core.sample(16 * GiB,
                    {
                        {.slot = 0, .memory = id * GiB}
        });
        core.finish(id);
    }
    ZEXPECT(core.scheduler.unknown_peak() == 4 * GiB);
}

};  // ZEST_SUITE(WorkerScheduler)

}  // namespace

}  // namespace clice::testing
