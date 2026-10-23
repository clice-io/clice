module;

#include "support/prelude.h"
#ifndef _WIN32
#include <signal.h>
#endif

module clice;

import :support.anomaly;
import :support.signal;
import :unittests.server.worker_test_helpers;
import :unittests.utils;
import :worker.pool;
import :worker.probe;
import :worker.protocol;
import :worker.scheduler;

namespace clice::testing {

/// Test fixture with friend access to WorkerPool internals.
struct WorkerPoolFixture {
    using SlotState = WorkerPool::SlotState;

    kota::event_loop loop;
    BuildProbe probe;
    WorkerPool pool;

    std::vector<WorkerCrashInfo> crash_reports;

    /// Set whenever a stateless slot (re)enters service.
    kota::event serving{false};
    Signal<>::Connection serving_connection;

    WorkerPoolFixture() : pool(loop) {
        // Unstarted, the pool schedules with the defaults; lift the
        // background ceiling so the slots a test adds bound it, and let
        // memory admit everything.
        pool.scheduler = WorkerScheduler({.background_jobs = 64, .max_workers = 64});
        sample();
        logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
        pool.on_crash = [this](const WorkerCrashInfo& info) {
            crash_reports.push_back(info);
        };
        serving_connection = pool.on_stateless_capacity.connect([this] { serving.set(); });
    }

    ~WorkerPoolFixture() {
        logging::reset_anomaly_for_testing();
    }

    /// A stateless slot with no process behind it; one alive is in the
    /// scheduler's service.
    void add_stateless(bool alive = true) {
        auto idx = pool.stateless_workers.size();
        pool.stateless_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateless_workers.back();
        w.name = "SL-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        if(alive) {
            pool.scheduler.worker_ready(idx, WorkerPool::now());
        }
    }

    void add_stateful(bool alive = true, std::size_t owned = 0) {
        auto idx = pool.stateful_workers.size();
        pool.stateful_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateful_workers.back();
        w.name = "SF-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        w.owned_documents = owned;
    }

    /// Bring a dying stateless slot back into service, as a respawn would.
    void respawn(std::size_t idx) {
        auto& w = pool.stateless_workers[idx];
        w.state = SlotState::Alive;
        w.death = std::make_shared<WorkerDeath>();
        pool.serving(idx, false);
    }

    /// Hand the scheduler a look at memory: plenty unless told otherwise,
    /// with `memory` the own memory of every stateless worker in service.
    void sample(std::uint64_t headroom = 1ull << 40, std::uint64_t memory = 0) {
        SchedulerSample sample{.headroom = headroom, .reserve = 512ull << 20};
        for(std::size_t idx = 0; idx < pool.stateless_workers.size(); idx += 1) {
            auto& w = pool.stateless_workers[idx];
            if(w.state == SlotState::Alive) {
                w.memory = memory;
                sample.workers.push_back({.slot = idx, .memory = memory});
            }
        }
        sample.stateless_processes = sample.workers.size();
        pool.scheduler.sample(std::move(sample), WorkerPool::now());
        pool.execute();
    }

    /// A stateless request submitted as send_stateless does, held until the
    /// pointer goes: its end.
    std::unique_ptr<WorkerPool::PendingStateless> submit(worker::Priority priority) {
        auto id = pool.next_request;
        pool.next_request += 1;
        auto request = std::make_unique<WorkerPool::PendingStateless>(pool, id);
        pool.scheduler.submit(id, priority, {}, true, WorkerPool::now());
        pool.execute();
        return request;
    }

    /// A request in flight on the slot, as a send registers it.
    std::unique_ptr<WorkerPool::Inflight> inflight(RequestId id, std::string tag, bool build) {
        return std::make_unique<WorkerPool::Inflight>(pool, id, std::move(tag), build);
    }

    /// A stateful request sent to slot `idx`, as dispatch_stateful
    /// registers it with the scheduler.
    RequestId stateful_request(std::size_t idx, bool build = false, std::uint32_t document = 0) {
        auto id = pool.next_request;
        pool.next_request += 1;
        pool.scheduler.stateful_begin(id, idx, document, build, WorkerPool::now());
        return id;
    }

    void kill(std::size_t idx,
              bool stateful,
              scheduler::KillReason reason,
              std::vector<RequestId> requests = {}) {
        pool.kill_worker(
            {.slot = idx, .stateful = stateful, .reason = reason, .requests = std::move(requests)});
    }

    /// As stop() leaves the pool before its workers are reaped.
    void mark_stopped() {
        pool.stop_scope.cancel();
    }

    void set_max_crash_streak(unsigned n) {
        pool.options.max_crash_streak = n;
    }

    void set_crash_streak(std::size_t idx, bool stateful, unsigned streak) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].crash_streak = streak;
    }

    void set_uptime(std::size_t idx, bool stateful, std::chrono::milliseconds uptime) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].spawn_time = std::chrono::steady_clock::now() - uptime;
    }

    std::size_t pick_least_loaded() {
        return pool.pick_least_loaded();
    }

    std::size_t assign_worker(std::uint32_t path_id) {
        return pool.assign_worker(path_id);
    }

    void remove_owner(std::uint32_t path_id) {
        pool.remove_owner(path_id);
    }

    /// A worker's own failure by default: SIGSEGV, not an outside kill.
    bool simulate_crash(std::size_t index, bool stateful, int exit_code = 0, int exit_signal = 11) {
        return pool.process_crash(index, stateful, exit_code, exit_signal);
    }

    void mark_dead(std::size_t idx, bool stateful = false) {
        pool.mark_worker_dead(idx, stateful, false);
    }

    void set_retiring(std::size_t idx) {
        pool.stateless_workers[idx].retiring = true;
    }

    /// The dying worker named the request it ran, as its crash report
    /// would.
    void set_culprit(std::size_t idx, bool stateful, std::string tag) {
        pool.slot(idx, stateful).death->culprits.push_back(std::move(tag));
    }

    /// The worker's last stderr, as its drain would have kept it.
    void set_stderr(std::size_t idx, std::string crashed_in, bool out_of_memory) {
        auto tail = std::make_shared<StderrTail>();
        tail->crashed_in = std::move(crashed_in);
        tail->out_of_memory = out_of_memory;
        pool.stateless_workers[idx].stderr_tail = std::move(tail);
    }

    std::shared_ptr<WorkerDeath> death(std::size_t idx, bool stateful) {
        return pool.slot(idx, stateful).death;
    }

    void set_deadlines(std::chrono::milliseconds build, std::chrono::milliseconds query) {
        pool.options.build_deadline = build;
        pool.options.query_deadline = query;
    }

    static ipc::Error death_error(const WorkerDeath& death, llvm::StringRef tag) {
        return WorkerPool::death_error(death, tag, "sf:0:1");
    }

    struct Delivery {
        int sends = 0;
        int blames = 0;
        bool answered = false;
    };

    /// deliver() over sends that fail with `codes` in turn and answer once
    /// the codes run out.
    Delivery deliver_through(std::vector<worker::protocol::integer> codes) {
        Delivery delivery;
        run([&]() -> kota::task<> {
            auto result = co_await deliver(
                pool,
                true,
                [&]() -> RequestResult<worker::HoverParams> {
                    auto attempt = static_cast<std::size_t>(delivery.sends);
                    delivery.sends += 1;
                    if(attempt < codes.size()) {
                        co_return kota::outcome_error(
                            worker::protocol::Error{codes[attempt], "scripted"});
                    }
                    co_return codec::RawValue{"null"};
                },
                [&](const ipc::Error&) { delivery.blames += 1; });
            delivery.answered = result.has_value();
        });
        return delivery;
    }

    void set_revive_after(std::chrono::milliseconds cooldown) {
        pool.options.revive_after = cooldown;
    }

    void set_max_stateless(std::size_t n) {
        pool.options.max_stateless = n;
    }

    bool slot_retired(std::size_t idx) const {
        return pool.stateless_workers[idx].state == WorkerPool::SlotState::Retired;
    }

    bool slot_dead(std::size_t idx, bool stateful = false) const {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        return workers[idx].state == WorkerPool::SlotState::Dead;
    }

    std::size_t stateless_count() const {
        return pool.stateless_workers.size();
    }

    void tick_oom_scores(std::uint64_t memory_limit) {
        pool.tick_oom_scores(memory_limit);
    }

    std::chrono::milliseconds backoff_delay(unsigned streak) {
        return pool.backoff_delay(streak);
    }

    bool scale_up() {
        return pool.scale_up_worker();
    }

    bool start(std::uint32_t stateless = 2,
               std::uint32_t stateful = 0,
               std::uint32_t min_stateless = 0) {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = stateless;
        opts.stateful_count = stateful;
        opts.min_stateless = min_stateless;
        opts.max_stateless = 8;
        return pool.start(opts);
    }

    std::size_t min_stateless() const {
        return pool.options.min_stateless;
    }

    kota::task<> stop() {
        return pool.stop();
    }

    template <typename F>
    void run(F&& coro_factory) {
        loop.schedule(coro_factory());
        loop.run();
    }

    void kill_worker(std::size_t idx, int signal = 9) {
        pool.stateless_workers[idx].proc.kill(signal);
    }

    /// The request a send in flight is known to the scheduler by; the pool
    /// holds no other.
    RequestId only_request() const {
        assert(pool.pending.size() == 1);
        return pool.pending.begin()->first;
    }

    static worker::TURunParams index_params(const std::string& file) {
        worker::TURunParams params;
        params.index = true;
        params.file = file;
        params.directory = "/tmp";
        params.arguments = make_args(file);
        return params;
    }

    /// Kill stateless worker 0 while it runs a request on `file`: a death
    /// that spends the slot's budget on every platform, Windows included,
    /// which can send a worker no signal of its own failures.
    kota::task<> kill_running_worker(const std::string& file) {
        auto params = index_params(file);
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            [[maybe_unused]] auto result =
                co_await pool.send_stateless(params, worker::Priority::High);
        };
        // The spawn runs the sender up to its wait for the reply.
        group.spawn(sender());
        ZASSERT(pool.scheduler.running_on(0, false) == 1u);
        kill_worker(0);
        co_await group.join();
    }

    std::size_t capacity() const {
        return pool.stateless_capacity();
    }

    std::size_t stateful_owned(std::size_t idx) const {
        return pool.stateful_workers[idx].owned_documents;
    }

    bool has_owner(std::uint32_t path_id) const {
        return pool.owner.count(path_id);
    }

    std::size_t owner_of(std::uint32_t path_id) const {
        return pool.owner.find(path_id)->second;
    }

    SlotState state(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].state : pool.stateless_workers[idx].state;
    }

    unsigned generation(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].generation
                        : pool.stateless_workers[idx].generation;
    }

    bool preempted(std::size_t idx) const {
        return pool.stateless_workers[idx].preempted;
    }

    unsigned crash_streak(std::size_t idx, bool stateful = false) const {
        return stateful ? pool.stateful_workers[idx].crash_streak
                        : pool.stateless_workers[idx].crash_streak;
    }

    bool worker_alive(std::size_t idx) const {
        return pool.stateless_workers[idx].state == SlotState::Alive;
    }

    /// Returns once stateless slot `idx` serves: spawned, respawned, revived
    /// or refilled.
    kota::task<> serves(std::size_t idx) {
        while(!worker_alive(idx)) {
            serving.reset();
            co_await serving.wait();
        }
    }

    /// Returns once the pool has handled the exit of the process now in
    /// stateless slot `idx`.
    kota::task<> exited(std::size_t idx) {
        auto death = pool.stateless_workers[idx].death;
        co_await death->done.wait();
    }

    /// Park the work of the next request like `params` where it starts in
    /// its worker (see BuildProbe::gate); returns the gate's id.
    template <typename Params>
    std::uint64_t gate(const Params& params) {
        pool.probe = &probe;
        return probe.gate(worker::crash_tag(params));
    }

    int worker_pid(std::size_t idx) const {
        return pool.stateless_workers[idx].proc.pid();
    }

    /// The stateless requests the scheduler runs on slot `idx`.
    std::size_t running_on(std::size_t idx) const {
        return pool.scheduler.running_on(idx, false);
    }

    bool pending(RequestId id) const {
        return pool.pending.contains(id);
    }

    void fail_pending() {
        pool.fail_pending_requests();
    }

    void retire(std::size_t idx) {
        pool.retire_worker(idx);
    }

    /// End the link to the worker as its own exit would, from this side.
    void close_link(std::size_t idx) {
        pool.stateless_workers[idx].peer->close();
    }

    /// Its sender cancelled the request.
    void cancel(RequestId id) {
        pool.scheduler.cancel(id, WorkerPool::now());
        pool.execute();
    }

    void sample_pool() {
        pool.sample();
    }

    std::uint64_t memory(std::size_t idx, bool stateful) {
        return pool.slot(idx, stateful).memory;
    }

    /// Index `file` on stateless worker 0, sampling it all along. Returns
    /// the most sampled.
    kota::task<std::uint64_t> index_sampled(const std::string& file) {
        std::uint64_t peak = 0;
        bool answered = false;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await pool.send_stateless(index_params(file), worker::Priority::High);
            ZEXPECT(result);
            answered = true;
        };
        auto sampler = [&]() -> kota::task<> {
            while(!answered) {
                sample_pool();
                peak = std::max(peak, memory(0, false));
                // No event: a worker's memory grows unannounced.
                co_await kota::sleep(std::chrono::milliseconds(50), loop);
            }
        };
        co_await kota::when_all(sender(), sampler());
        co_return peak;
    }
};

namespace {

constexpr std::uint64_t MiB = 1ull << 20;

/// A unit big enough to tell in its worker's memory: indexing it reaches
/// 320 to 560 MiB.
std::string big_unit(TempDir& tmp) {
    std::string values = "int values[] = {";
    for(int i = 0; i < 2'000'000; i += 1) {
        values += std::to_string(i);
        values += ',';
    }
    values += "};\n";
    tmp.touch("big.cpp", values);
    return tmp.path("big.cpp");
}

ZEST_SUITE(WorkerPoolStateful) {

ZEST_CASE(PickLeastLoaded) {
    WorkerPoolFixture f;
    f.add_stateful(true, 5);
    f.add_stateful(true, 2);
    f.add_stateful(true, 8);
    ZEXPECT(f.pick_least_loaded() == 1u);
}

ZEST_CASE(SkipDeadWorkers) {
    WorkerPoolFixture f;
    f.add_stateful(false, 0);
    f.add_stateful(true, 5);
    f.add_stateful(true, 3);
    ZEXPECT(f.pick_least_loaded() == 2u);
}

ZEST_CASE(StaleEvictionIgnored) {
    WorkerPoolFixture f;
    f.add_stateful();
    f.add_stateful();
    auto idx = f.assign_worker(7);

    // An eviction from a worker that lost ownership (probe reassignment
    // left it a stale copy) must not unseat the current owner.
    ZEXPECT(!f.pool.remove_owner_from(7, idx + 1));
    ZEXPECT(f.has_owner(7));

    ZEXPECT(f.pool.remove_owner_from(7, idx));
    ZEXPECT(!f.has_owner(7));
    ZEXPECT(f.stateful_owned(idx) == 0u);
}

ZEST_CASE(AllDeadNoAssignment) {
    WorkerPoolFixture f;
    f.add_stateful(false, 0);
    f.add_stateful(false, 0);
    ZEXPECT(f.pick_least_loaded() == SIZE_MAX);
    ZEXPECT(f.assign_worker(100) == SIZE_MAX);
    // A failed assignment must not pin the document to a dead worker.
    ZEXPECT(!f.has_owner(100));
}

ZEST_CASE(AssignNewPath) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto idx = f.assign_worker(100);
    ZEXPECT(f.stateful_owned(idx) == 1u);
    ZEXPECT(f.has_owner(100));
    ZEXPECT(f.owner_of(100) == idx);
}

ZEST_CASE(PathAffinity) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto a = f.assign_worker(100);
    auto b = f.assign_worker(100);
    ZEXPECT(a == b);
    ZEXPECT(f.stateful_owned(a) == 1u);
}

ZEST_CASE(LoadBalancing) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto a = f.assign_worker(100);
    auto b = f.assign_worker(200);
    ZEXPECT(a != b);
}

ZEST_CASE(RemoveOwner) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.assign_worker(100);
    ZEXPECT(f.stateful_owned(0) == 1u);
    f.remove_owner(100);
    ZEXPECT(f.stateful_owned(0) == 0u);
    ZEXPECT(!f.has_owner(100));
}

ZEST_CASE(RemoveNonexistent) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.remove_owner(999);
}

ZEST_CASE(DeathClearsOwner) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    f.assign_worker(100);
    f.assign_worker(200);
    f.assign_worker(300);
    ZEXPECT(f.stateful_owned(0) == 2u);
    f.mark_dead(0, true);
    ZEXPECT(f.stateful_owned(0) == 0u);
    ZEXPECT(!f.has_owner(100));
    ZEXPECT(!f.has_owner(300));
    ZEXPECT(f.has_owner(200));
}

ZEST_CASE(RebalanceAfterClose) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    f.assign_worker(100);
    f.assign_worker(200);
    f.assign_worker(300);
    ZEXPECT(f.stateful_owned(0) == 2u);
    ZEXPECT(f.stateful_owned(1) == 1u);

    // Closing documents shrinks the owner table and load counts.
    f.remove_owner(100);
    f.remove_owner(300);
    ZEXPECT(f.stateful_owned(0) == 0u);
    ZEXPECT(!f.has_owner(100));
    ZEXPECT(!f.has_owner(300));

    // New assignments go to the now least-loaded worker.
    ZEXPECT(f.assign_worker(400) == 0u);
    ZEXPECT(f.stateful_owned(0) == 1u);
}

};  // ZEST_SUITE(WorkerPoolStateful)

ZEST_SUITE(WorkerPoolActions) {

ZEST_CASE(DispatchFillsSlot) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::High);
    ZEXPECT(request->ready.is_set());
    ZEXPECT(request->slot == 0u);
    ZEXPECT(request->generation == f.generation(0));
    ZEXPECT(f.running_on(0) == 1u);
}

ZEST_CASE(FinishDispatchesNext) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto first = f.submit(worker::Priority::Low);
    auto second = f.submit(worker::Priority::Low);
    ZEXPECT(first->slot.has_value());
    ZEXPECT(!second->ready.is_set());
    first.reset();
    ZEXPECT(second->slot == 0u);
}

ZEST_CASE(QueuedEndLeavesQueue) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto running = f.submit(worker::Priority::Low);
    auto queued = f.submit(worker::Priority::Low);
    auto id = queued->id;
    queued.reset();
    ZEXPECT(!f.pending(id));
    // The slot that frees finds nothing left to run.
    running.reset();
    ZEXPECT(f.running_on(0) == 0u);
}

ZEST_CASE(StaleEndSparesSlot) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto stale = f.submit(worker::Priority::High);
    ZASSERT(stale->slot == 0u);
    // The worker dies before its request's sender resumes; the request
    // after it runs on the respawned worker.
    f.mark_dead(0);
    f.respawn(0);
    auto next = f.submit(worker::Priority::High);
    ZASSERT(next->slot == 0u);
    stale.reset();
    ZEXPECT(f.running_on(0) == 1u);
    next.reset();
    ZEXPECT(f.running_on(0) == 0u);
}

ZEST_CASE(ForegroundCancelsBackground) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto background = f.submit(worker::Priority::Low);
    ZASSERT(background->slot.has_value());
    auto foreground = f.submit(worker::Priority::High);
    ZEXPECT(!foreground->ready.is_set());
    ZEXPECT(background->preempt.cancelled());
    background.reset();
    ZEXPECT(foreground->slot == 0u);
}

ZEST_CASE(FailWakesQueuedOnly) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto running = f.submit(worker::Priority::High);
    auto queued = f.submit(worker::Priority::High);
    running->ready.reset();
    f.fail_pending();
    ZEXPECT(queued->ready.is_set());
    ZEXPECT(!queued->slot.has_value());
    ZEXPECT(!running->ready.is_set());
}

ZEST_CASE(PulseNarrowsBackground) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    ZEXPECT(f.pool.background_limit() == 64u);
    f.pool.foreground_pulse();
    ZEXPECT(f.pool.background_limit() == 1u);
}

ZEST_CASE(MemoryKillRecordsDeath) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    f.sample(1ull << 40, 3ull << 30);
    auto death = f.death(0, false);
    f.kill(0, false, scheduler::KillReason::Memory, {request->id});

    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.preempted(0));
    ZEXPECT(death->out_of_memory);
    ZEXPECT(death->memory == 3ull << 30);
    ZEXPECT(death->alone);
    ZEXPECT(death->culprits.empty());
    // The scheduler forgot the request with its worker.
    ZEXPECT(f.running_on(0) == 0u);
    // A kill on purpose is not a crash: no report, no crash accounting.
    ZEXPECT(f.crash_reports.empty());
    ZEXPECT(f.crash_streak(0) == 0u);
}

ZEST_CASE(MemoryKillKeepsPeak) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    f.sample(1ull << 40, 3ull << 30);
    f.sample(1ull << 40, 2ull << 30);
    auto death = f.death(0, false);
    f.kill(0, false, scheduler::KillReason::Memory, {request->id});
    ZEXPECT(death->memory == 3ull << 30);
}

ZEST_CASE(CompanyIsNotAlone) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto background = f.submit(worker::Priority::Low);
    auto foreground = f.submit(worker::Priority::High);
    auto death = f.death(0, false);
    f.kill(0, false, scheduler::KillReason::Memory, {background->id});
    ZEXPECT(death->out_of_memory);
    ZEXPECT(!death->alone);
}

ZEST_CASE(KillCreditsHealthyRun) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    auto young = f.submit(worker::Priority::Low);
    f.set_crash_streak(0, false, 2);
    f.set_uptime(0, false, std::chrono::milliseconds(60'000));
    f.set_crash_streak(1, false, 2);
    f.set_uptime(1, false, std::chrono::milliseconds(0));

    f.kill(0, false, scheduler::KillReason::Memory, {request->id});
    f.kill(1, false, scheduler::KillReason::CancelGrace, {young->id});

    // The healthy interval before the kill clears the stale streak, exactly
    // as the next crash's accounting would have; a slot that has not
    // earned it keeps its streak: a kill on purpose neither punishes nor
    // forgives.
    ZEXPECT(f.crash_streak(0) == 0u);
    ZEXPECT(f.crash_streak(1) == 2u);
}

ZEST_CASE(GraceKillIsLost) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    auto death = f.death(0, false);
    f.kill(0, false, scheduler::KillReason::CancelGrace, {request->id});
    ZEXPECT(f.preempted(0));
    ZEXPECT(death->ignored_cancel);
    ZEXPECT(!death->out_of_memory);
    ZEXPECT(death->culprits.empty());
    // A run that never stopped spends its sender's retries.
    ZEXPECT(WorkerPoolFixture::death_error(*death, "clice/worker/tuRun /a.cpp").code ==
            worker::dispatch_errc::worker_lost);
}

ZEST_CASE(DeadlineKillNames) {
    WorkerPoolFixture f;
    f.set_deadlines(std::chrono::seconds(10), std::chrono::seconds(1));
    f.add_stateful();
    auto query = f.stateful_request(0);
    auto hover = f.inflight(query, "clice/worker/hover /b.cpp", false);
    auto other = f.stateful_request(0, false, 1);
    auto links = f.inflight(other, "clice/worker/documentLink /c.cpp", false);
    auto death = f.death(0, true);
    // One kill names every overdue request of the worker.
    f.kill(0, true, scheduler::KillReason::Deadline, {query, other});
    ZEXPECT(f.state(0, true) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(death->culprits.size() == 2u);
    ZEXPECT(WorkerPoolFixture::death_error(*death, "clice/worker/hover /b.cpp").code ==
            worker::dispatch_errc::worker_crashed);
    ZEXPECT(WorkerPoolFixture::death_error(*death, "clice/worker/documentLink /c.cpp").code ==
            worker::dispatch_errc::worker_crashed);
    ZEXPECT(f.death(0, true)->cause == "killed after running for over 1 seconds");

    f.add_stateless();
    f.add_stateless();
    auto run = f.submit(worker::Priority::Low);
    auto index = f.inflight(run->id, "clice/worker/tuRun /c.cpp", true);
    f.kill(0, false, scheduler::KillReason::Deadline, {run->id});
    ZEXPECT(f.death(0, false)->culprits[0] == "clice/worker/tuRun /c.cpp");
    ZEXPECT(f.death(0, false)->cause == "killed after over 10 seconds of CPU time");
    ZEXPECT(!f.preempted(0));

    auto stuck = f.submit(worker::Priority::Low);
    ZASSERT(stuck->slot == 1u);
    auto stalled = f.inflight(stuck->id, "clice/worker/tuRun /d.cpp", true);
    f.kill(1, false, scheduler::KillReason::Stalled, {stuck->id});
    ZEXPECT(f.death(1, false)->culprits[0] == "clice/worker/tuRun /d.cpp");
    ZEXPECT(f.death(1, false)->cause == "killed after 300 seconds without using the CPU");
}

ZEST_CASE(AllocationFailureIsBlameless) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    f.set_crash_streak(0, false, 1);
    // The abort an allocation failure ends in names its request.
    f.set_stderr(0, "clice/worker/tuRun /a.cpp", true);
    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    ZEXPECT(f.simulate_crash(0, false, 0, 6));

    auto death = f.death(0, false);
    ZEXPECT(death->out_of_memory);
    ZEXPECT(death->alone);
    ZEXPECT(death->culprits.empty());
    ZEXPECT(death->cause == "ran out of memory");
    ZEXPECT(f.crash_streak(0) == 1u);
    ZEXPECT(trapped.empty());
}

ZEST_CASE(StoppedPoolSchedulesNothing) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    ZEXPECT(f.pool.schedulable_stateless() == 2u);
    f.mark_stopped();
    ZEXPECT(f.pool.schedulable_stateless() == 0u);
}

};  // ZEST_SUITE(WorkerPoolActions)

ZEST_SUITE(WorkerPoolCrash) {

ZEST_CASE(StatelessCleanup) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    ZASSERT(request->slot == 0u);

    f.simulate_crash(0, false);

    ZEXPECT(f.pool.schedulable_stateless() == 1u);
    ZEXPECT(f.running_on(0) == 0u);
}

ZEST_CASE(StatefulLostDocuments) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);

    // Deterministic: first two go to worker 0 (least loaded), third to worker 1
    auto w0 = f.assign_worker(100);
    auto w1 = f.assign_worker(200);
    f.assign_worker(300);
    ZASSERT(w0 != w1);

    // Record which docs worker 0 owns before crashing it
    llvm::SmallVector<std::uint32_t> expected;
    for(auto id: {100u, 200u, 300u}) {
        if(f.owner_of(id) == 0)
            expected.push_back(id);
    }

    f.simulate_crash(0, true);

    ZASSERT(f.crash_reports.size() == 1u);
    auto& report = f.crash_reports[0];
    ZEXPECT(report.stateful);
    ZEXPECT(report.worker_index == 0u);
    ZEXPECT(report.lost_documents.size() == expected.size());

    for(auto path_id: expected) {
        ZEXPECT(!f.has_owner(path_id));
        ZEXPECT(ranges::contains(report.lost_documents, path_id));
    }

    // Other worker's documents are preserved
    for(auto id: {100u, 200u, 300u}) {
        if(!ranges::contains(expected, id))
            ZEXPECT(f.has_owner(id));
    }
}

ZEST_CASE(CrashReportsAnomaly) {
    /// Production trigger for the WorkerCrash anomaly: process_crash is the
    /// exact site the pool reports from.
    WorkerPoolFixture f;
    f.add_stateless();

    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    f.simulate_crash(0, false, 0, 11);

    ZASSERT(trapped.size() == 1u);
    ZEXPECT(trapped[0] == logging::AnomalyId::WorkerCrash);
}

ZEST_CASE(SpawnFailReportsAnomaly) {
    /// Production trigger for the WorkerSpawnFail anomaly.
    WorkerPoolFixture f;

    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    WorkerPoolOptions opts;
    opts.self_path = "/nonexistent/clice-binary";
    opts.stateless_count = 1;
    opts.stateful_count = 0;
    ZEXPECT(!f.pool.start(opts));

    ZASSERT(trapped.size() == 1u);
    ZEXPECT(trapped[0] == logging::AnomalyId::WorkerSpawnFail);
}

ZEST_CASE(OperationalErrorCodes) {
    /// Dispatch failures marked operational must never classify as anomalies.
    using worker::protocol::Error;
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::cancelled, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::worker_unavailable, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::worker_crashed, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::worker_lost, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::worker_died, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::document_unloaded, "x"}));
    ZEXPECT(worker::is_operational_error(Error{worker::dispatch_errc::out_of_memory, "x"}));
    ZEXPECT(!worker::is_operational_error(Error{"plain failure"}));
}

ZEST_CASE(TransportErrorClassification) {
    /// A closed link is ConnectionClosed; remote handler errors carry other
    /// codes and must pass through.
    using worker::protocol::Error;
    using worker::protocol::ErrorCode;
    ZEXPECT(worker::is_transport_error(Error{ErrorCode::ConnectionClosed, "peer closed"}));
    ZEXPECT(!worker::is_transport_error(Error{"handler failed"}));
    ZEXPECT(!worker::is_transport_error(Error{ErrorCode::InternalError, "handler threw"}));
    ZEXPECT(!worker::is_transport_error(Error{ErrorCode::RequestCancelled, "cancelled"}));
}

ZEST_CASE(CrashInfoSignal) {
    WorkerPoolFixture f;
    f.add_stateless();

    f.simulate_crash(0, false, 0, 11);

    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(f.crash_reports[0].exit_signal == 11);
    ZEXPECT(f.crash_reports[0].exit_code == 0);
    ZEXPECT(!f.crash_reports[0].stateful);
}

ZEST_CASE(CrashInfoExitCode) {
    WorkerPoolFixture f;
    f.add_stateless();

    f.simulate_crash(0, false, 42, 0);

    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(f.crash_reports[0].exit_code == 42);
    ZEXPECT(f.crash_reports[0].exit_signal == 0);
}

ZEST_CASE(WillRestartWithinBudget) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 1);

    auto should_restart = f.simulate_crash(0, false);

    ZEXPECT(should_restart);
    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(f.crash_reports[0].will_restart);
    ZEXPECT(f.crash_reports[0].crash_streak == 2u);
}

ZEST_CASE(CrashBudgetExhausted) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 3);

    auto should_restart = f.simulate_crash(0, false);

    ZEXPECT(!should_restart);
    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(!f.crash_reports[0].will_restart);
    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dead);
}

ZEST_CASE(HealthyUptimeResetsStreak) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 3);
    // Ran healthily past the reset threshold: this crash starts a new streak
    // instead of exhausting the budget.
    f.set_uptime(0, false, std::chrono::milliseconds(60'000));

    auto should_restart = f.simulate_crash(0, false);

    ZEXPECT(should_restart);
    ZEXPECT(f.crash_streak(0) == 1u);
}

ZEST_CASE(NamedCrashKeepsBudget) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.set_crash_streak(0, true, 2);
    f.set_culprit(0, true, "clice/worker/compile /a.cpp");

    // A death that names its request is that request's content's doing:
    // the streak stays where it was and the slot respawns.
    auto should_restart = f.simulate_crash(0, true);

    ZEXPECT(should_restart);
    ZEXPECT(f.crash_streak(0, true) == 2u);
}

ZEST_CASE(IdleKillKeepsBudget) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(3);

    // A terminated session, or an idle worker the OOM killer or a user
    // killed, says nothing about the slot: SIGHUP, SIGINT, SIGKILL and
    // SIGTERM in a row leave the budget whole.
    for(int signal: {1, 2, 9, 15}) {
        ZEXPECT(f.simulate_crash(0, false, 0, signal));
    }
    ZEXPECT(f.crash_streak(0) == 0u);

    f.simulate_crash(0, false, 0, 11);
    ZEXPECT(f.crash_streak(0) == 1u);
}

ZEST_CASE(BusyKillSpendsBudget) {
    WorkerPoolFixture f;
    f.add_stateful(true);

    // Killed while compiling several documents, none of which is blamed:
    // the budget alone slows a load that keeps getting killed.
    f.stateful_request(0, true);
    f.stateful_request(0, true);
    f.simulate_crash(0, true, 0, 9);
    ZEXPECT(f.crash_streak(0, true) == 1u);

    // So does one killed between requests while it holds documents.
    f.add_stateful(true);
    ZASSERT(f.assign_worker(1) == 1u);
    f.simulate_crash(1, true, 0, 9);
    ZEXPECT(f.crash_streak(1, true) == 1u);
}

ZEST_CASE(DeathNamesItsRequest) {
    WorkerDeath death;
    death.cause = "killed by signal 11 (SIGSEGV)";
    death.culprits = {"clice/worker/compile /a.cpp"};

    using namespace worker::dispatch_errc;
    auto own = WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp");
    ZEXPECT(own.code == worker_crashed);
    ZEXPECT(own.message == death.cause);
    ZEXPECT(worker::death_of(own) == "sf:0:1");
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /b.cpp").code ==
            worker_lost);

    death.culprits.clear();
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp").code ==
            worker_died);

    // Among several requests a nameless death accuses none of them.
    death.in_flight = 2;
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp").code ==
            worker_lost);
}

ZEST_CASE(MemoryErrorCarriesData) {
    WorkerDeath death;
    death.cause = "killed to free memory";
    death.out_of_memory = true;
    death.memory = 3ull << 30;
    death.alone = true;
    death.in_flight = 1;

    auto error = WorkerPoolFixture::death_error(death, "clice/worker/tuRun /a.cpp");
    ZEXPECT(error.code == worker::dispatch_errc::out_of_memory);
    ZEXPECT(error.message == death.cause);
    ZASSERT(error.data.has_value());
    ZEXPECT((*error.data)["memory"].get_uint() == 3ull << 30);
    ZEXPECT((*error.data)["alone"].get_bool() == true);
}

ZEST_CASE(DeathCountsCompany) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.add_stateful(true);
    f.stateful_request(0, true);
    f.stateful_request(1, true);
    f.stateful_request(1, true);
    auto lone = f.death(0, true);
    auto shared = f.death(1, true);

    f.mark_dead(0, true);
    f.mark_dead(1, true);

    ZEXPECT(lone->in_flight == 1u);
    ZEXPECT(shared->in_flight == 2u);
}

ZEST_CASE(DeathFreesDocuments) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.add_stateful(true);
    ZASSERT(f.assign_worker(1) == 0u);

    // A request routed in the window before the crash report must not
    // reach the corpse: the death itself frees the document.
    f.mark_dead(0, true);
    ZEXPECT(f.assign_worker(1) == 1u);

    f.simulate_crash(0, true);
    ZASSERT(f.crash_reports.size() == 1u);
    ZASSERT(f.crash_reports[0].lost_documents.size() == 1u);
    ZEXPECT(f.crash_reports[0].lost_documents[0] == 1u);
}

ZEST_CASE(DeliverBlamesOwnCrash) {
    WorkerPoolFixture f;
    auto delivery = f.deliver_through({worker::dispatch_errc::worker_crashed});
    ZEXPECT(delivery.sends == 1);
    ZEXPECT(delivery.blames == 1);
    ZEXPECT(!delivery.answered);
}

ZEST_CASE(DeliverResendsVictimsOnce) {
    using namespace worker::dispatch_errc;
    // A death of another request's doing, one that named no request, or
    // one of memory running out is resent once.
    for(auto code: {worker_lost, worker_died, out_of_memory}) {
        WorkerPoolFixture f;
        auto delivery = f.deliver_through({code});
        ZEXPECT(delivery.sends == 2);
        ZEXPECT(delivery.blames == 0);
        ZEXPECT(delivery.answered);
    }
    // Only once, and a second death blames the request only when neither
    // named one.
    for(auto codes: {
            std::vector{worker_lost, worker_lost},
            std::vector{worker_died, worker_lost},
            std::vector{worker_lost, worker_died}
    }) {
        WorkerPoolFixture f;
        auto delivery = f.deliver_through(codes);
        ZEXPECT(delivery.sends == 2);
        ZEXPECT(delivery.blames == 0);
        ZEXPECT(!delivery.answered);
    }
    WorkerPoolFixture f;
    auto delivery = f.deliver_through({worker_died, worker_died});
    ZEXPECT(delivery.sends == 2);
    ZEXPECT(delivery.blames == 1);
    ZEXPECT(!delivery.answered);
}

ZEST_CASE(DeliverWaitsForCapacity) {
    using worker::dispatch_errc::worker_unavailable;
    {
        // No slot will ever serve again: the request gives up.
        WorkerPoolFixture f;
        auto delivery = f.deliver_through({worker_unavailable});
        ZEXPECT(delivery.sends == 1);
        ZEXPECT(!delivery.answered);
    }
    // A serving slot: capacity windows are waited out, not counted as
    // resends.
    WorkerPoolFixture f;
    f.add_stateful(true);
    auto delivery = f.deliver_through({worker_unavailable, worker_unavailable});
    ZEXPECT(delivery.sends == 3);
    ZEXPECT(delivery.blames == 0);
    ZEXPECT(delivery.answered);
}

ZEST_CASE(BackoffDelaySchedule) {
    WorkerPoolFixture f;
    using ms = std::chrono::milliseconds;
    // First crash respawns immediately; later ones back off exponentially,
    // capped so a long streak cannot produce absurd delays.
    ZEXPECT(f.backoff_delay(0) == ms(0));
    ZEXPECT(f.backoff_delay(1) == ms(0));
    ZEXPECT(f.backoff_delay(2) == ms(500));
    ZEXPECT(f.backoff_delay(3) == ms(1000));
    ZEXPECT(f.backoff_delay(4) == ms(2000));
    ZEXPECT(f.backoff_delay(10) == ms(8000));
}

ZEST_CASE(MarkDeadRemovesSlot) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    ZASSERT(request->slot == 0u);

    f.mark_dead(0);

    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.generation(0) == 1u);
    ZEXPECT(f.running_on(0) == 0u);
    // The dying slot still counts as future capacity (its result is
    // pending), but is not schedulable: the next request goes elsewhere.
    ZEXPECT(f.capacity() == 2u);
    ZEXPECT(f.submit(worker::Priority::Low)->slot == 1u);

    // Idempotent: a second mark (e.g. the monitor's) is a no-op.
    f.mark_dead(0);
    ZEXPECT(f.generation(0) == 1u);
}

ZEST_CASE(EndAfterCrash) {
    WorkerPoolFixture f;
    f.add_stateless();
    auto request = f.submit(worker::Priority::Low);
    f.simulate_crash(0, false);
    // The scheduler forgot the request with its worker: its end, as its
    // sender unwinds, changes nothing.
    ZEXPECT(request->finish() == 0u);
    ZEXPECT(f.running_on(0) == 0u);
}

ZEST_CASE(StatefulCrashClearsOwnership) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    f.assign_worker(10);
    f.assign_worker(20);
    f.assign_worker(30);

    auto idx0 = f.owner_of(10);
    auto other_idx = idx0 == 0 ? 1u : 0u;
    auto other_owned_before = f.stateful_owned(other_idx);

    f.simulate_crash(idx0, true);

    ZEXPECT(!f.has_owner(10));
    ZEXPECT(f.stateful_owned(idx0) == 0u);
    ZEXPECT(f.stateful_owned(other_idx) == other_owned_before);
}

ZEST_CASE(IdleStatelessCrash) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();

    f.simulate_crash(0, false);

    ZEXPECT(f.pool.schedulable_stateless() == 1u);
    ZEXPECT(!f.worker_alive(0));
}

ZEST_CASE(RapidCrashSequence) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    auto first = f.submit(worker::Priority::Low);
    auto second = f.submit(worker::Priority::Low);

    f.simulate_crash(0, false);
    f.simulate_crash(1, false);
    f.simulate_crash(2, false);

    ZEXPECT(f.pool.schedulable_stateless() == 0u);
    ZEXPECT(f.crash_reports.size() == 3u);
}

ZEST_CASE(DeadPoolReturnsError) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(0);

    f.simulate_crash(0, false);
    ZEXPECT(f.capacity() == 0u);

    std::optional<worker::protocol::integer> code;
    f.run([&]() -> kota::task<> {
        auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params("/a.cpp"),
                                                     worker::Priority::High);
        ZASSERT(!result.has_value());
        code = result.error().code;
    });
    ZEXPECT(code == worker::dispatch_errc::worker_unavailable);
}

ZEST_CASE(DeadPoolWakesQueued) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(0);
    auto running = f.submit(worker::Priority::High);
    auto queued = f.submit(worker::Priority::High);
    ZASSERT(!queued->ready.is_set());

    f.simulate_crash(0, false);

    // The last slot gave up: the request learns it can never run.
    ZEXPECT(queued->ready.is_set());
    ZEXPECT(!queued->slot.has_value());
}

ZEST_CASE(CrashBudgetBoundary) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 2);

    // Exactly at the budget: the streak reaches 3 == max, still restarts.
    ZEXPECT(f.simulate_crash(0, false));
    ZEXPECT(f.crash_streak(0) == 3u);

    // One past the budget: give up.
    ZEXPECT(!f.simulate_crash(0, false));
    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dead);
}

ZEST_CASE(StatefulDyingUnavailable) {
    // A request arriving in the restart window never reaches a worker: the
    // death freed its document, and with no other worker alive it fails
    // with worker_unavailable — no death for crash accounting to read.
    WorkerPoolFixture f;
    f.add_stateful();
    f.assign_worker(7);
    f.mark_dead(0, true);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto result = co_await f.pool.send_stateful(7, worker::DocumentLinkParams{"/x.cpp"});
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::worker_unavailable);
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(DyingSlotKeepsWaiters) {
    // A slot marked dead but without a result still counts as future
    // capacity: a queued request keeps waiting for the respawn instead of
    // failing out.
    WorkerPoolFixture f;
    f.add_stateless();
    auto running = f.submit(worker::Priority::High);
    auto queued = f.submit(worker::Priority::High);

    f.mark_dead(0);
    ZEXPECT(!queued->ready.is_set());
    f.respawn(0);
    ZEXPECT(queued->slot == 0u);
}

};  // ZEST_SUITE(WorkerPoolCrash)

ZEST_SUITE(WorkerPoolIntegration) {

ZEST_CASE(StartAndStop) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 1));
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

#ifndef _WIN32
ZEST_CASE(StopReapsWorkers) {
    // stop() returns once every worker has exited and been reaped: a stop
    // that let go of the exit observers would return with them running.
    WorkerPoolFixture f;
    std::vector<int> errors;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        std::vector<int> pids = {f.worker_pid(0), f.worker_pid(1)};
        co_await f.stop();
        for(auto pid: pids) {
            errors.push_back(::kill(pid, 0) == 0 ? 0 : errno);
        }
    });
    ZEXPECT(errors == std::vector<int>{ESRCH, ESRCH});
}
#endif

ZEST_CASE(StoppedPoolBlamesNothing) {
    // A stopped pool's stateful workers are still Alive with their links
    // closed: a request routed to one would die nameless on every attempt
    // and be blamed for killing its worker.
    WorkerPoolFixture f;
    int blames = 0;
    std::optional<worker::protocol::integer> code;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(0, 2));
        co_await f.stop();
        auto result = co_await deliver(
            f.pool,
            true,
            [&] { return f.pool.send_stateful(7, worker::DocumentLinkParams{"/x.cpp"}); },
            [&](const ipc::Error&) { blames += 1; });
        ZASSERT(!result.has_value());
        code = result.error().code;
    });
    ZEXPECT(blames == 0);
    ZEXPECT(code == worker::dispatch_errc::worker_unavailable);
}

ZEST_CASE(StatelessRequest) {
    TempDir tmp;
    tmp.touch("test.cpp", "int x = 1;\n");
    auto src = tmp.path("test.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(src),
                                                     worker::Priority::High);
        ZEXPECT(result);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(AdvisoryCancelCooperates) {
    // s#1: an advisory-token fire mid-build cancels the request over
    // IPC. The sender keeps awaiting the real reply — the slot
    // frees only once the worker is actually idle — and the result is
    // classified as a cancellation, with the worker healthy afterwards.
    TempDir tmp;
    tmp.touch("first.cpp", "int x = 1;\n");
    auto first_src = tmp.path("first.cpp");
    tmp.touch("small.cpp", "int y = 2;\n");
    auto small_src = tmp.path("small.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));

        auto params = WorkerPoolFixture::index_params(first_src);

        kota::cancellation_source advisory;
        bool cancelled = false;
        auto gate = f.gate(params);
        auto sender = [&]() -> kota::task<> {
            auto result =
                co_await f.pool.send_stateless(params, worker::Priority::High, advisory.token());
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        // The build is under way in the worker when the token fires.
        auto driver = [&]() -> kota::task<> {
            co_await parked(f.probe, gate);
            advisory.cancel();
            f.probe.release(gate);
        };
        co_await kota::when_all(sender(), driver());
        ZEXPECT(cancelled);

        // The worker replied and survived: the same slot serves the next
        // request.
        auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(small_src),
                                                     worker::Priority::High);
        ZEXPECT(result);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(CrashAndRestart) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        auto pid = f.worker_pid(0);
        ZEXPECT(pid > 0);

        f.kill_worker(0);
        co_await f.exited(0);
        co_await f.serves(0);
        ZEXPECT(f.worker_pid(0) != pid);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(DeadSlotRevives) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        f.set_revive_after(std::chrono::milliseconds(300));
        f.set_max_crash_streak(0);

        // The very first crash exceeds the zero budget: the slot dies...
        co_await f.kill_running_worker(tmp.path("slow.cpp"));
        ZEXPECT(f.slot_dead(0));

        // ...and is revived with a fresh budget after the cooldown: the
        // pool never stays at zero workers forever.
        co_await f.serves(0);
        ZEXPECT(f.crash_streak(0) == 0u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(FloorAboveStartupKept) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        // A floor configured above the startup count survives start():
        // scale-up may grow past it later, and idle scale-down must hold
        // the configured warm set instead of shrinking back to startup.
        ZASSERT(f.start(2, 0, /*min_stateless=*/4));
        ZEXPECT(f.min_stateless() == 4u);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

#ifdef __linux__
ZEST_CASE(IdleKeepsSessionScore) {
    auto score = [](llvm::StringRef pid) {
        auto file = llvm::MemoryBuffer::getFileAsStream("/proc/" + pid + "/oom_score_adj");
        return file ? (*file)->getBuffer().trim().str() : std::string();
    };
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        // An idle worker ranks like the master for the OOM killer, never
        // ahead of everything else on the machine (#773).
        auto own = score("self");
        ZASSERT(!own.empty());
        ZEXPECT(score(std::to_string(f.worker_pid(0))) == own);
        ZEXPECT(score(std::to_string(f.worker_pid(1))) == own);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(BusyRanksAboveMaster) {
    auto score = [](const std::string& pid) {
        auto file = llvm::MemoryBuffer::getFileAsStream("/proc/" + pid + "/oom_score_adj");
        int value = -2000;
        if(file && (*file)->getBuffer().trim().getAsInteger(10, value)) {
            value = -2000;
        }
        return value;
    };
    WorkerPoolFixture f;
    auto worker_score = [&](std::size_t idx) {
        return score(std::to_string(f.worker_pid(idx)));
    };
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        auto own = score("self");
        ZASSERT(own != -2000);

        // Taking on work ranks a worker above the master at once, not only
        // from the next tick on.
        ZASSERT(own < 1000);
        auto request = f.submit(worker::Priority::High);
        ZASSERT(request->slot.has_value());
        auto idx = *request->slot;
        ZEXPECT(worker_score(idx) > own);

        // Against a limit so large the master's share rounds to nothing, a
        // worker holding work ranks just above the master; an idle one
        // stays level with it.
        constexpr std::uint64_t huge = 1ull << 50;
        f.tick_oom_scores(huge);
        ZEXPECT(worker_score(idx) == std::min(own + 2, 1000));
        ZEXPECT(worker_score(1 - idx) == own);

        // Done, it drops back at once.
        request.reset();
        ZEXPECT(worker_score(idx) == own);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}
#endif

ZEST_CASE(RetiringHoldsScaleUp) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        f.set_max_stateless(2);
        f.set_retiring(0);

        // The retiring worker still holds its process until the monitor
        // reaps it: a replacement now would run three processes against a
        // ceiling of two, worsening the pressure that retired it.
        ZEXPECT(!f.scale_up());
        ZEXPECT(f.stateless_count() == 2u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(RevivesSlotsGate) {
    // Revival — and with it the indexer's requeue-on-unavailable — is only
    // promised by a started pool with a nonzero cooldown.
    WorkerPoolFixture f;
    ZEXPECT(!f.pool.revives_slots());

    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        ZEXPECT(f.pool.revives_slots());
        f.set_revive_after(std::chrono::milliseconds(0));
        ZEXPECT(!f.pool.revives_slots());
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(ScaleUpRevivesDead) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        // Cooldown far in the future: only scale-up can bring the slot back.
        f.set_revive_after(std::chrono::minutes(10));
        f.set_max_crash_streak(0);

        co_await f.kill_running_worker(tmp.path("slow.cpp"));
        ZASSERT(f.slot_dead(0));

        // Scale-up pressure revives the dead slot instead of appending a
        // third worker next to it.
        ZASSERT(f.scale_up());
        ZEXPECT(f.stateless_count() == 2u);
        co_await f.serves(0);
        ZEXPECT(f.crash_streak(0) == 0u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(ScaleUpReusesRetired) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        f.retire(1);
        co_await f.exited(1);
        ZASSERT(f.slot_retired(1));

        // The retired slot is refilled instead of a third appended: a
        // retire/scale-up cycle must not grow the slot table.
        ZASSERT(f.scale_up());
        ZEXPECT(f.stateless_count() == 2u);
        co_await f.serves(1);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(CrashNotification) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        f.kill_worker(0);
        co_await f.exited(0);

        ZASSERT(!f.crash_reports.empty());
        ZEXPECT(!f.crash_reports[0].stateful);
        ZEXPECT(f.crash_reports[0].exit_signal == 9);
        ZEXPECT(f.crash_reports[0].will_restart);
        // An idle worker's kill spends no crash budget.
        ZEXPECT(f.crash_reports[0].crash_streak == 0u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(CrashDuringRequest) {
    TempDir tmp;
    tmp.touch("test.cpp", "int x = 1;\n");
    auto src = tmp.path("test.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        // Kill both workers, then send without yielding: the dispatch goes
        // to a dead-but-not-yet-reaped slot, and the pool must report the
        // death instead of retrying internally — worker_died, since an
        // outside kill names no request.
        f.kill_worker(0);
        f.kill_worker(1);

        auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(src),
                                                     worker::Priority::High);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::worker_died);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(MemoryKillReportsMemory) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        std::optional<ipc::Error> error;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(src),
                                                         worker::Priority::High);
            if(!result.has_value())
                error = result.error();
        };
        // The spawn runs the sender up to its wait for the reply; killed
        // within the same loop turn, the request is caught in flight.
        group.spawn(sender());
        ZASSERT(f.running_on(0) == 1u);
        f.kill(0, false, scheduler::KillReason::Memory, {f.only_request()});
        co_await group.join();
        ZASSERT(error.has_value());
        ZEXPECT(error->code == worker::dispatch_errc::out_of_memory);
        ZASSERT(error->data.has_value());
        ZEXPECT((*error->data)["alone"].get_bool() == true);

        // A kill on purpose is not a crash: no report, and the worker comes
        // back with an untouched budget.
        ZEXPECT(f.crash_reports.empty());
        co_await f.serves(0);
        ZEXPECT(f.crash_streak(0) == 0u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(MemoryKillBeatsCancel) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        kota::cancellation_source source;
        worker::protocol::integer code = 0;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(src),
                                                         worker::Priority::High,
                                                         source.token());
            if(!result.has_value())
                code = result.error().code;
        };
        group.spawn(sender());
        ZASSERT(f.running_on(0) == 1u);
        // A memory kill arriving before the cancelled request stopped is
        // reported as such: its sender learns what the request reached.
        source.cancel();
        f.kill(0, false, scheduler::KillReason::Memory, {f.only_request()});
        co_await group.join();
        ZEXPECT(code == worker::dispatch_errc::out_of_memory);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(CancelledCrashRetiresSlot) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));

        worker::protocol::integer code = 0;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(WorkerPoolFixture::index_params(src),
                                                         worker::Priority::High);
            if(!result.has_value())
                code = result.error().code;
        };
        group.spawn(sender());
        ZASSERT(f.running_on(0) == 1u);

        // A cooperative cancel whose victim then dies on its own inside the
        // grace window: the request still classifies as cancelled, but the
        // slot must be retired with it — released as Alive it would take
        // the next dispatch before the monitor observes the exit. The
        // generation bump is the retirement evidence that survives an
        // instant respawn.
        auto gen = f.generation(0);
        f.cancel(f.only_request());
        f.kill_worker(0);
        co_await group.join();
        ZEXPECT(code == worker::dispatch_errc::cancelled);
        ZEXPECT(f.generation(0) != gen);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

#ifndef _WIN32
ZEST_CASE(LinkEndMarksDead) {
    // A worker whose link ends is dead before its exit is observed: a
    // request dispatched to it meanwhile would die nameless. A stopped
    // worker cannot exit at all.
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        auto gen = f.generation(0);
        f.kill_worker(0, SIGSTOP);
        f.close_link(0);
        while(f.generation(0) == gen) {
            co_await kota::yield();
        }
        co_await f.serves(0);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}
#endif

ZEST_CASE(MemoryKillReachesSender) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");

    WorkerPoolFixture f;
    std::optional<ipc::Error> error;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        auto params = WorkerPoolFixture::index_params(tmp.path("slow.cpp"));
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(params, worker::Priority::High);
            if(!result.has_value()) {
                error = result.error();
            }
        };
        group.spawn(sender());
        ZASSERT(f.running_on(0) == 1u);
        f.kill(0, false, scheduler::KillReason::Memory);
        co_await group.join();
        // Back in service at once, its budget untouched.
        co_await f.serves(0);
        ZEXPECT(f.crash_streak(0) == 0u);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
    ZASSERT(error.has_value());
    ZEXPECT(error->code == worker::dispatch_errc::out_of_memory);
    ZASSERT(error->data.has_value());
    ZEXPECT((*error->data)["alone"].get_bool() == true);
}

ZEST_CASE(SampleReadsWorkers) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 1));
        f.sample_pool();
        ZEXPECT(f.memory(0, false) > 0u);
        ZEXPECT(f.memory(1, false) > 0u);
        ZEXPECT(f.memory(0, true) > 0u);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(IdleWorkerGivesBack) {
    // A worker that kept what its request freed would hold the high-water
    // mark of its biggest request while idle.
    TempDir tmp;
    auto src = big_unit(tmp);

    WorkerPoolFixture f;
    std::uint64_t before = 0;
    std::uint64_t peak = 0;
    [[maybe_unused]] std::uint64_t after = 0;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        f.sample_pool();
        before = f.memory(0, false);
        peak = co_await f.index_sampled(src);
        f.sample_pool();
        after = f.memory(0, false);
        co_await f.stop();
    });
    ZEXPECT(peak > before + 128 * MiB);
    // ASan's allocator keeps what is freed in its quarantine; the release
    // build's allocators are the ones that must give it back.
#if !__has_feature(address_sanitizer)
    ZEXPECT(after < before + 64 * MiB);
#endif
}

ZEST_CASE(ReusedMemoryStillCounted) {
    // The second request reuses what the first freed: a worker's memory
    // that missed reused pages would read it as a small unit.
    TempDir tmp;
    auto src = big_unit(tmp);

    WorkerPoolFixture f;
    std::uint64_t before = 0;
    std::uint64_t second = 0;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));
        f.sample_pool();
        before = f.memory(0, false);
        co_await f.index_sampled(src);
        second = co_await f.index_sampled(src);
        co_await f.stop();
    });
    ZEXPECT(second > before + 128 * MiB);
}

};  // ZEST_SUITE(WorkerPoolIntegration)

}  // namespace

}  // namespace clice::testing
