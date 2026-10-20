module;

#include "modules/prelude.h"

module clice:unittests.worker.simulator;

import :unittests.worker.workloads;
import :worker.scheduler;

namespace clice::testing {

constexpr inline std::uint64_t MiB = 1ull << 20;

/// The machine a simulated pool runs on.
struct SimMachine {
    std::uint64_t memory = 16384 * MiB;

    /// What the rest of the machine uses, from each second on.
    std::vector<std::pair<double, std::uint64_t>> others = {
        {0, 1450 * MiB}
    };

    std::uint32_t physical = 2;

    std::uint32_t logical = 4;

    /// Past the limit the kernel kills the largest worker, as Linux does
    /// without swap; otherwise every worker slows, as on macOS's compressor.
    bool oom_kills = true;

    /// macOS keeps a larger reserve: decompression starts near 20% free.
    bool mac_reserve = false;
};

/// The user typing from `from` to `until`: an edit and a completion
/// request every `every` seconds.
struct SimTyping {
    double from = 0;

    double until = 0;

    double every = 2;

    /// The completion's CPU seconds and the peak of its worker's memory.
    double seconds = 0.3;

    std::uint32_t peak = 300;
};

struct SimSpec {
    SimMachine machine;

    SchedulerOptions scheduler;

    std::vector<UnitLoad> units;

    std::vector<SimTyping> typing;

    /// The units' peaks are known from their last run.
    bool known_peaks = false;
};

struct SimResult {
    double wall = 0;

    std::size_t completed = 0;

    /// Units killed for memory while they ran alone: too big for the
    /// machine.
    std::vector<std::size_t> too_big;

    /// Units given up otherwise: a deadline, or killed by the kernel more
    /// often than the requeue budget allows.
    std::vector<std::size_t> given_up;

    /// The units killed for memory, once for each kill.
    std::vector<std::size_t> memory_killed;

    std::size_t oom_kills = 0;

    std::size_t deadline_kills = 0;

    std::size_t cancels = 0;

    /// Unit-seconds of progress kills and cancels threw away.
    double lost = 0;

    /// The least headroom the machine had, in bytes; negative past the
    /// limit.
    std::int64_t min_headroom = std::numeric_limits<std::int64_t>::max();

    /// How long each completion waited for a worker.
    std::vector<double> foreground_waits;

    /// No schedule finishes sooner: the CPU work over the most the cores
    /// give, the longest unit, and the memory the units hold over time over
    /// the memory left to them.
    double lower_bound = 0;
};

namespace sim {

/// What a worker's own memory is at rest.
constexpr inline std::uint64_t base = 150 * MiB;

/// A worker's own memory over a unit, as measured: a straight line to 80%
/// of the peak while it parses, the rest in its last 15%, the index.
inline std::uint64_t memory_at(std::uint64_t peak, double progress) {
    if(peak <= base) {
        return base;
    }
    auto shape = progress < 0.85 ? 0.8 * progress / 0.85 : 0.8 + 0.2 * (progress - 0.85) / 0.15;
    return base +
           static_cast<std::uint64_t>(static_cast<double>(peak - base) * std::min(shape, 1.0));
}

/// What k busy workers get done together, in cores: a second thread on a
/// core adds 30%.
inline double throughput(const SimMachine& machine, std::size_t k) {
    auto on_cores = std::min<double>(k, machine.physical);
    auto on_threads = std::min<double>(k, machine.logical) - on_cores;
    return on_cores + 0.3 * on_threads;
}

inline std::uint64_t others_at(const SimMachine& machine, double t) {
    std::uint64_t others = 0;
    for(auto& [from, bytes]: machine.others) {
        if(t >= from) {
            others = bytes;
        }
    }
    return others;
}

inline std::uint64_t reserve(const SimMachine& machine) {
    if(machine.mac_reserve) {
        return std::max<std::uint64_t>(1536 * MiB, machine.memory / 10);
    }
    return std::max<std::uint64_t>(512 * MiB, machine.memory / 20);
}

}  // namespace sim

/// Run the units through a WorkerScheduler as the pool and the index pump
/// would: the pump keeps twice the background limit in flight and requeues
/// a lost run into the next round; the pool carries out every action, a
/// process takes 0.3 s to start and 0.2 s to die, a cancelled compile stops
/// within 0.2 s.
inline SimResult simulate(const SimSpec& spec) {
    using namespace std::chrono;
    constexpr double dt = 0.05;
    constexpr double start_delay = 0.3;
    constexpr double exit_delay = 0.2;
    constexpr double cancel_delay = 0.2;
    constexpr std::size_t max_lost = 3;

    auto& machine = spec.machine;
    SimResult result;
    WorkerScheduler core(spec.scheduler);
    auto at = [](double t) {
        return SchedulerTime{} + duration_cast<steady_clock::duration>(duration<double>(t));
    };

    struct Job {
        RequestId id = 0;

        /// The unit, or none for a completion.
        std::optional<std::size_t> unit;

        double seconds = 0;

        std::uint64_t peak = 0;

        double progress = 0;

        /// When a cancelled run stops.
        std::optional<double> stops;
    };

    struct Proc {
        bool alive = false;

        std::optional<double> ready_at;

        std::optional<double> exit_at;

        bool respawn = false;

        std::optional<scheduler::Kill> kill;

        /// A memory kill's: its memory then, and whether it ran alone.
        std::uint64_t killed_at = 0;

        bool alone = false;

        std::optional<Job> job;

        double cpu = 0;
    };

    std::vector<Proc> procs(spec.scheduler.background_jobs);
    for(auto& proc: procs) {
        proc.ready_at = 0;
    }

    struct Waiting {
        std::optional<std::size_t> unit;

        double submitted = 0;
    };

    std::map<RequestId, Waiting> waiting;
    RequestId next_id = 1;
    std::deque<std::size_t> queue;
    std::deque<std::size_t> next_round;
    for(std::size_t i = 0; i < spec.units.size(); i += 1) {
        queue.push_back(i);
    }
    std::vector<std::uint64_t> known(spec.units.size(), 0);
    if(spec.known_peaks) {
        for(std::size_t i = 0; i < spec.units.size(); i += 1) {
            known[i] = std::max<std::uint64_t>(spec.units[i].peak, sim::base / MiB) * MiB;
        }
    }
    std::vector<std::uint64_t> floors(spec.units.size(), 0);
    std::vector<std::size_t> lost_runs(spec.units.size(), 0);
    std::size_t in_flight = 0;
    std::size_t finished = 0;

    std::vector<double> typing_times;
    for(auto& typing: spec.typing) {
        for(double t = typing.from; t < typing.until; t += typing.every) {
            typing_times.push_back(t);
        }
    }
    std::ranges::sort(typing_times);
    std::size_t next_typing = 0;
    std::size_t open_completions = 0;

    double t = 0;
    double next_sample = 0;

    auto used = [&] {
        auto bytes = sim::others_at(machine, t);
        for(auto& proc: procs) {
            if(proc.alive) {
                bytes += proc.job ? sim::memory_at(proc.job->peak, proc.job->progress) : sim::base;
            }
        }
        return bytes;
    };

    auto requeue = [&](std::size_t unit) {
        next_round.push_back(unit);
    };

    auto give_up = [&](std::size_t unit, std::vector<std::size_t>& list) {
        list.push_back(unit);
        finished += 1;
    };

    auto end_job = [&](Job& job) {
        if(job.unit) {
            in_flight -= 1;
        } else {
            open_completions -= 1;
        }
    };

    auto run = [&] {
        // Like the pool, which takes the actions its own reports to the
        // scheduler cause before it returns.
        for(auto actions = core.take_actions(); !actions.empty(); actions = core.take_actions()) {
            for(auto& action: actions) {
                if(auto* dispatch = std::get_if<scheduler::Dispatch>(&action)) {
                    auto& waited = waiting.at(dispatch->request);
                    Job job{.id = dispatch->request, .unit = waited.unit};
                    if(waited.unit) {
                        job.seconds = spec.units[*waited.unit].seconds;
                        job.peak = std::uint64_t(spec.units[*waited.unit].peak) * MiB;
                    } else {
                        auto& typing = spec.typing.front();
                        job.seconds = typing.seconds;
                        job.peak = std::uint64_t(typing.peak) * MiB;
                        result.foreground_waits.push_back(t - waited.submitted);
                    }
                    waiting.erase(dispatch->request);
                    procs[dispatch->slot].job = job;
                } else if(auto* cancel = std::get_if<scheduler::Cancel>(&action)) {
                    for(auto& proc: procs) {
                        if(proc.job && proc.job->id == cancel->request && !proc.job->stops) {
                            proc.job->stops = t + cancel_delay;
                        }
                    }
                } else if(auto* kill = std::get_if<scheduler::Kill>(&action)) {
                    auto& proc = procs[kill->slot];
                    proc.kill = *kill;
                    proc.killed_at = sim::memory_at(proc.job->peak, proc.job->progress);
                    proc.alone = core.runs_alone(kill->slot);
                    proc.exit_at = t + exit_delay;
                    proc.respawn = true;
                    core.worker_gone(kill->slot, false, at(t));
                } else if(std::holds_alternative<scheduler::Spawn>(action)) {
                    auto vacant = std::ranges::find_if(procs, [](const Proc& proc) {
                        return !proc.alive && !proc.ready_at && !proc.exit_at;
                    });
                    if(vacant == procs.end()) {
                        procs.emplace_back();
                        vacant = procs.end() - 1;
                    }
                    vacant->ready_at = t + start_delay;
                } else if(auto* retire = std::get_if<scheduler::Retire>(&action)) {
                    auto& proc = procs[retire->slot];
                    proc.exit_at = t + 0.1;
                    proc.respawn = false;
                }
            }
        }
    };

    while(finished < spec.units.size() || next_typing < typing_times.size() ||
          open_completions > 0) {
        if(t > 50000) {
            break;
        }
        auto now = at(t);

        for(std::size_t slot = 0; slot < procs.size(); slot += 1) {
            auto& proc = procs[slot];
            if(proc.ready_at && *proc.ready_at <= t) {
                proc.ready_at.reset();
                proc.alive = true;
                proc.cpu = 0;
                core.worker_ready(slot, now);
                run();
            }
            if(proc.exit_at && *proc.exit_at <= t) {
                proc.exit_at.reset();
                proc.alive = false;
                if(proc.job) {
                    auto job = *proc.job;
                    proc.job.reset();
                    end_job(job);
                    if(job.unit) {
                        auto unit = *job.unit;
                        auto kill = proc.kill;
                        // The run that shows a unit too big is not lost: no
                        // schedule spares it.
                        if(!kill || kill->reason != scheduler::KillReason::Memory || !proc.alone) {
                            result.lost += job.progress * job.seconds;
                        }
                        if(kill && kill->reason == scheduler::KillReason::Memory) {
                            result.memory_killed.push_back(unit);
                            if(proc.alone) {
                                give_up(unit, result.too_big);
                            } else {
                                floors[unit] = std::max(floors[unit], proc.killed_at);
                                requeue(unit);
                            }
                        } else if(kill && (kill->reason == scheduler::KillReason::Deadline ||
                                           kill->reason == scheduler::KillReason::Stalled)) {
                            give_up(unit, result.given_up);
                        } else if(kill && kill->reason == scheduler::KillReason::CancelGrace) {
                            requeue(unit);
                        } else {
                            lost_runs[unit] += 1;
                            if(lost_runs[unit] > max_lost) {
                                give_up(unit, result.given_up);
                            } else {
                                requeue(unit);
                            }
                        }
                    }
                }
                if(proc.kill) {
                    auto reason = proc.kill->reason;
                    result.deadline_kills += reason == scheduler::KillReason::Deadline ||
                                             reason == scheduler::KillReason::Stalled;
                }
                if(!proc.kill) {
                    core.worker_gone(slot, false, now);
                }
                proc.kill.reset();
                core.worker_exited(slot);
                run();
                if(proc.respawn) {
                    proc.respawn = false;
                    proc.ready_at = t + start_delay;
                }
            }
            if(proc.job && proc.job->stops && *proc.job->stops <= t) {
                auto job = *proc.job;
                proc.job.reset();
                end_job(job);
                result.cancels += 1;
                if(job.unit) {
                    result.lost += job.progress * job.seconds;
                    requeue(*job.unit);
                }
                core.finish(job.id, now);
                run();
            }
        }

        while(next_typing < typing_times.size() && typing_times[next_typing] <= t) {
            next_typing += 1;
            core.user_activity(now);
            run();
            auto id = next_id;
            next_id += 1;
            waiting[id] = {.submitted = t};
            open_completions += 1;
            core.submit(id, worker::Priority::High, {}, true, now);
            run();
        }

        if(queue.empty() && in_flight == 0 && !next_round.empty()) {
            queue.assign(next_round.begin(), next_round.end());
            next_round.clear();
        }
        auto window = 2 * core.background_limit(now);
        while(!queue.empty() && in_flight < window) {
            auto unit = queue.front();
            queue.pop_front();
            auto id = next_id;
            next_id += 1;
            waiting[id] = {.unit = unit, .submitted = t};
            in_flight += 1;
            core.submit(id,
                        worker::Priority::Low,
                        {.peak = known[unit], .floor = floors[unit]},
                        true,
                        now);
            run();
        }

        if(t >= next_sample) {
            SchedulerSample sample;
            auto bytes = used();
            sample.headroom = bytes < machine.memory ? machine.memory - bytes : 0;
            sample.reserve = sim::reserve(machine);
            for(std::size_t slot = 0; slot < procs.size(); slot += 1) {
                auto& proc = procs[slot];
                if(proc.alive || proc.ready_at || proc.exit_at) {
                    sample.stateless_processes += 1;
                }
                // The pool samples the workers in service.
                if(proc.alive && !proc.kill) {
                    sample.workers.push_back({
                        .slot = slot,
                        .memory = proc.job ? sim::memory_at(proc.job->peak, proc.job->progress)
                                           : sim::base,
                        .cpu = milliseconds(static_cast<std::int64_t>(proc.cpu * 1000)),
                    });
                }
            }
            core.sample(std::move(sample), now);
            run();
            next_sample = t + duration<double>(core.sample_interval()).count();
        }

        // A killed process does no more work.
        auto working = [](const Proc& proc) {
            return proc.alive && proc.job && !proc.job->stops && !proc.kill;
        };
        std::size_t busy = std::ranges::count_if(procs, working);
        if(busy > 0) {
            auto bytes = used();
            auto factor = 1.0;
            if(!machine.oom_kills && bytes > machine.memory) {
                auto over = double(bytes - machine.memory) / double(machine.memory);
                factor = std::max(0.05, 1 - 3 * over);
            }
            auto speed = sim::throughput(machine, busy) / double(busy);
            for(auto& proc: procs) {
                if(!working(proc)) {
                    continue;
                }
                proc.cpu += speed * dt;
                proc.job->progress += speed * factor * dt / proc.job->seconds;
            }
        }
        for(auto& proc: procs) {
            if(proc.alive && proc.job && proc.job->progress >= 1) {
                auto job = *proc.job;
                proc.job.reset();
                end_job(job);
                auto peak = core.finish(job.id, at(t));
                if(job.unit) {
                    known[*job.unit] = peak;
                    result.completed += 1;
                    finished += 1;
                }
                run();
            }
        }

        auto bytes = used();
        result.min_headroom =
            std::min(result.min_headroom, std::int64_t(machine.memory) - std::int64_t(bytes));
        if(machine.oom_kills && bytes > machine.memory) {
            auto largest = std::ranges::max_element(procs, {}, [](const Proc& proc) {
                return proc.alive && proc.job ? sim::memory_at(proc.job->peak, proc.job->progress)
                                              : 0;
            });
            if(largest->alive && largest->job && !largest->exit_at) {
                largest->exit_at = t;
                largest->respawn = true;
                result.oom_kills += 1;
            }
        }
        t += dt;
    }

    result.wall = t;
    double work = 0;
    double longest = 0;
    double held = 0;
    for(auto& unit: spec.units) {
        work += unit.seconds;
        longest = std::max(longest, unit.seconds);
        held += double(sim::base + std::uint64_t(unit.peak) * MiB) / 2 * unit.seconds;
    }
    auto jobs = std::min<std::size_t>(spec.scheduler.background_jobs, machine.logical);
    auto room = double(machine.memory - sim::others_at(machine, 0) - sim::reserve(machine));
    result.lower_bound = std::max({work / sim::throughput(machine, jobs), longest, held / room});
    return result;
}

}  // namespace clice::testing
