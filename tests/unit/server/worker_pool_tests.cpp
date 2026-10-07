module;

#include "modules/prelude.h"
#ifndef _WIN32
#include <signal.h>
#endif

module clice;

import :support.anomaly;
import :tests.unit.server.worker_test_helpers;
import :tests.unit.test.test;
import :worker.pool;
import :worker.protocol;

namespace clice::testing {

/// Test fixture with friend access to WorkerPool internals.
struct WorkerPoolFixture {
    using SlotState = WorkerPool::SlotState;

    kota::event_loop loop;
    WorkerPool pool;

    std::vector<WorkerCrashInfo> crash_reports;

    WorkerPoolFixture() : pool(loop) {
        // Unstarted, the pool keeps WorkerPoolOptions' defaults; lift the
        // configured background ceiling so the slots a test adds bound it.
        pool.options.stateless_count = 64;
        logging::set_anomaly_trap_for_testing([](logging::AnomalyId) {});
        pool.on_crash = [this](const WorkerCrashInfo& info) {
            crash_reports.push_back(info);
        };
    }

    ~WorkerPoolFixture() {
        logging::reset_anomaly_for_testing();
    }

    void add_stateless(bool alive = true, bool busy = false, bool low = true) {
        auto idx = pool.stateless_workers.size();
        pool.stateless_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateless_workers.back();
        w.name = "SL-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        w.busy = busy;
        w.low_priority = busy && low;
    }

    void add_stateful(bool alive = true, std::size_t owned = 0) {
        auto idx = pool.stateful_workers.size();
        pool.stateful_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateful_workers.back();
        w.name = "SF-" + std::to_string(idx);
        w.state = alive ? SlotState::Alive : SlotState::Dead;
        w.owned_documents = owned;
    }

    void set_low_limit(std::size_t low) {
        pool.low_limit = low;
    }

    /// Arm a busy slot with a live cancellation source, as a resumed
    /// sender would; cancel_low_priority only victimizes armed slots.
    std::shared_ptr<kota::cancellation_source> arm_cancel_source(std::size_t idx) {
        auto source = std::make_shared<kota::cancellation_source>();
        pool.stateless_workers[idx].preempt_source = source;
        return source;
    }

    void tick_foreground() {
        pool.tick_foreground();
    }

    /// Age the last foreground activity past the hold window.
    void rewind_fg_activity() {
        pool.last_fg_activity =
            std::chrono::steady_clock::now() - WorkerPool::fg_hold - std::chrono::seconds(1);
    }

    /// Stamp a cooperative cancel as having happened past the grace window.
    void expire_cancel_grace(std::size_t idx) {
        pool.stateless_workers[idx].cancel_requested_at =
            std::chrono::steady_clock::now() - WorkerPool::cancel_grace - std::chrono::seconds(1);
    }

    void tick_cancel_grace() {
        pool.tick_cancel_grace();
    }

    void set_claim_epoch(std::size_t idx, std::uint64_t epoch) {
        pool.stateless_workers[idx].claim_epoch = epoch;
    }

    void cancel_low(std::size_t count) {
        pool.cancel_low_priority(count);
    }

    bool cancel_asked(std::size_t idx) {
        return pool.stateless_workers[idx].cancel_requested_at !=
               std::chrono::steady_clock::time_point{};
    }

    std::size_t reclaim_deficit(std::size_t pending_high = 0) {
        return pool.low_reclaim_deficit(pending_high);
    }

    /// Queue a fake High waiter, as acquire_stateless_slot would when no
    /// slot is idle; the deficit counts it as demand.
    void queue_high() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        queued_high.push_back(std::move(pending));
    }

    std::vector<std::unique_ptr<WorkerPool::PendingStateless>> queued_high;

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

    std::size_t pick_idle() {
        return pool.pick_idle_stateless();
    }

    void release_slot(std::size_t idx) {
        pool.release_stateless_slot(idx);
    }

    kota::task<std::size_t> acquire_slot(worker::Priority p, kota::cancellation_token cancel = {}) {
        return pool.acquire_stateless_slot(p, cancel);
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
        pool.slot(idx, stateful).death->culprit = std::move(tag);
    }

    std::shared_ptr<WorkerDeath> death(std::size_t idx, bool stateful) {
        return pool.slot(idx, stateful).death;
    }

    /// A request in flight on the slot since `age` ago.
    std::unique_ptr<WorkerPool::Dispatch> dispatch(std::size_t idx,
                                                   bool stateful,
                                                   std::string tag,
                                                   std::chrono::milliseconds age = {},
                                                   bool build = false) {
        auto dispatch =
            std::make_unique<WorkerPool::Dispatch>(pool, idx, stateful, std::move(tag), build);
        dispatch->started -= age;
        return dispatch;
    }

    void set_deadlines(std::chrono::milliseconds build, std::chrono::milliseconds query) {
        pool.options.build_deadline = build;
        pool.options.query_deadline = query;
    }

    void tick_deadlines() {
        pool.tick_deadlines();
    }

    static kota::ipc::Error death_error(const WorkerDeath& death, llvm::StringRef tag) {
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
                [&]() -> RequestResult<worker::QueryParams> {
                    auto attempt = static_cast<std::size_t>(delivery.sends);
                    delivery.sends += 1;
                    if(attempt < codes.size()) {
                        co_return kota::outcome_error(
                            worker::protocol::Error{codes[attempt], "scripted"});
                    }
                    co_return kota::codec::RawValue{"null"};
                },
                [&](const kota::ipc::Error&) { delivery.blames += 1; });
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

    void retire_idle() {
        pool.retire_idle_worker();
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

    /// Manually queue a waiter, as acquire_stateless_slot does when it
    /// cannot claim directly.
    auto enqueue_waiter(worker::Priority p) {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, p);
        auto& queue = p == worker::Priority::High ? pool.high_queue : pool.low_queue;
        queue.push_back(pending.get());
        pending->queue = &queue;
        return pending;
    }

    void dispatch_pending() {
        pool.try_dispatch_pending();
    }

    void preempt(std::size_t count) {
        pool.preempt_low_priority(count);
    }

    void tick_memory(double ratio) {
        pool.tick_memory(ratio);
    }

    void tick_scaling(double ratio) {
        pool.tick_scaling(ratio);
    }

    unsigned saturated_cycles() const {
        return pool.saturated_cycles;
    }

    void tick_oom_scores(std::uint64_t memory_limit) {
        pool.tick_oom_scores(memory_limit);
    }

    static auto find_cgroup_memory(llvm::StringRef proc_cgroup) {
        return WorkerPool::find_cgroup_memory(proc_cgroup);
    }

    static std::optional<std::uint64_t> cgroup_available(std::string usage,
                                                         std::string stat,
                                                         std::uint64_t limit) {
        return WorkerPool::cgroup_available({std::move(usage), std::move(stat), "inactive_file"},
                                            limit);
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

    void force_owner(std::uint32_t path_id, std::size_t idx) {
        pool.owner[path_id] = idx;
        pool.stateful_workers[idx].owned_documents += 1;
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

    /// Kill stateless worker 0 while it runs a request on `file`: a death
    /// that spends the slot's budget on every platform, Windows included,
    /// which can send a worker no signal of its own failures.
    kota::task<> kill_running_worker(const std::string& file) {
        worker::TURunParams params;
        params.index = true;
        params.file = file;
        params.directory = "/tmp";
        params.arguments = make_args(file);
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            [[maybe_unused]] auto result =
                co_await pool.send_stateless(params, worker::Priority::Low);
        };
        // The spawn runs the sender up to its wait for the reply.
        group.spawn(sender());
        ZASSERT(pool.stateless_workers[0].busy);
        kill_worker(0);
        co_await group.join();
    }

    void set_stateless_count(std::uint32_t count) {
        pool.options.stateless_count = count;
    }

    std::size_t low_limit() const {
        return pool.low_limit;
    }

    std::size_t effective_low_limit() const {
        return pool.effective_low_limit();
    }

    std::size_t max_low_limit() const {
        return pool.max_low_limit();
    }

    std::size_t alive_count() const {
        return pool.alive_stateless();
    }

    std::size_t busy_count() const {
        return pool.busy_stateless();
    }

    std::size_t low_busy() const {
        return pool.low_busy_count();
    }

    std::size_t capacity() const {
        return pool.stateless_capacity();
    }

    std::size_t stateful_owned(std::size_t idx) const {
        return pool.stateful_workers[idx].owned_documents;
    }

    bool is_busy(std::size_t idx) const {
        return pool.stateless_workers[idx].busy;
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

    int worker_pid(std::size_t idx) const {
        return pool.stateless_workers[idx].proc.pid();
    }

    std::size_t high_queue_size() const {
        return pool.high_queue.size();
    }

    std::size_t low_queue_size() const {
        return pool.low_queue.size();
    }

    struct PriorityResult {
        bool high_dispatched;
        bool low_dispatched;
    };

    PriorityResult test_priority_dispatch(std::size_t release_idx) {
        auto high = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        auto low = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::Low);
        pool.high_queue.push_back(high.get());
        high->queue = &pool.high_queue;
        pool.low_queue.push_back(low.get());
        low->queue = &pool.low_queue;
        pool.release_stateless_slot(release_idx);
        return {high->ready.is_set(), low->ready.is_set()};
    }

    struct LowDispatchResult {
        std::size_t dispatched;

        /// Busy count observed while the waiters still hold their claims;
        /// destroying an unconsumed dispatched waiter releases its slot.
        std::size_t busy_at_dispatch;
    };

    LowDispatchResult test_low_dispatch(std::size_t count) {
        llvm::SmallVector<std::unique_ptr<WorkerPool::PendingStateless>> pending;
        for(std::size_t i = 0; i < count; ++i) {
            pending.push_back(
                std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::Low));
            pool.low_queue.push_back(pending.back().get());
            pending.back()->queue = &pool.low_queue;
        }
        pool.try_dispatch_pending();
        std::size_t dispatched = 0;
        for(auto& p: pending)
            if(p->ready.is_set())
                dispatched += 1;
        return {dispatched, pool.busy_stateless()};
    }

    bool test_slot_raii(std::size_t idx) {
        pool.stateless_workers[idx].busy = true;
        { WorkerPool::StatelessSlot slot(pool, idx); }
        return !pool.stateless_workers[idx].busy && pool.busy_stateless() == 0 &&
               pool.low_busy_count() == 0;
    }

    struct ReleaseResult {
        bool pending_before;
        bool dispatched_after;
    };

    ReleaseResult test_release_dispatches() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        bool before = pending->ready.is_set();
        pool.release_stateless_slot(0);
        bool after = pending->ready.is_set();
        return {before, after};
    }

    bool test_pending_cleanup_high() {
        {
            WorkerPool::PendingStateless pending(pool, worker::Priority::High);
            pool.high_queue.push_back(&pending);
            pending.queue = &pool.high_queue;
            if(pool.high_queue.size() != 1)
                return false;
        }
        return pool.high_queue.empty();
    }

    bool test_pending_cleanup_low() {
        {
            WorkerPool::PendingStateless pending(pool, worker::Priority::Low);
            pool.low_queue.push_back(&pending);
            pending.queue = &pool.low_queue;
            if(pool.low_queue.size() != 1)
                return false;
        }
        return pool.low_queue.empty();
    }

    struct DispatchResult {
        bool dispatched;
        bool queue_cleared;
        bool queue_ptr_nulled;
    };

    struct DrainResult {
        bool drained;
        std::size_t assigned_worker;
    };

    DrainResult test_dead_pool_drain() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        simulate_crash(0, false);
        return {pending->ready.is_set(), pending->assigned_worker};
    }

    bool test_slot_gen_guard() {
        // Old StatelessSlot should NOT release a slot whose occupant died and
        // was replaced (generation bumped) in the meantime.
        {
            WorkerPool::StatelessSlot slot(pool, 0);
            pool.stateless_workers[0].generation += 1;
            pool.stateless_workers[0].busy = true;
        }
        return pool.stateless_workers[0].busy && pool.busy_stateless() == 1;
    }

    bool test_pending_gen_guard() {
        // A dispatched-but-unconsumed waiter must NOT release the slot if the
        // claimed occupant died and a new claim owns it now.
        auto pending = enqueue_waiter(worker::Priority::High);
        pool.try_dispatch_pending();
        if(!pending->ready.is_set() || pending->assigned_worker != 0)
            return false;
        pool.stateless_workers[0].generation += 1;
        pool.stateless_workers[0].busy = true;
        pending.reset();
        return pool.stateless_workers[0].busy;
    }

    DispatchResult test_dispatch_clears_queue() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(pool, worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        pool.release_stateless_slot(0);
        return {
            pending->ready.is_set(),
            pool.high_queue.empty(),
            pending->queue == nullptr,
        };
    }
};

namespace {

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

ZEST_SUITE(WorkerPoolScheduling) {

ZEST_CASE(PickIdleBasic) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    ZEXPECT(f.pick_idle() == 1u);
}

ZEST_CASE(PickIdleSkipsDead) {
    WorkerPoolFixture f;
    f.add_stateless(false, false);
    f.add_stateless(true, false);
    ZEXPECT(f.pick_idle() == 1u);
}

ZEST_CASE(PickIdleNoneAvailable) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(false, false);
    ZEXPECT(f.pick_idle() == SIZE_MAX);
}

ZEST_CASE(AcquireHighImmediate) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        ZEXPECT(f.is_busy(idx));
        ZEXPECT(f.busy_count() == 1u);
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(AcquireLowImmediate) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::Low);
        ZEXPECT(f.is_busy(idx));
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(HighBypassesLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(0);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        ZEXPECT(f.is_busy(idx));
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(HighDispatchedFirst) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_priority_dispatch(0);
    ZEXPECT(r.high_dispatched);
    ZEXPECT(!r.low_dispatched);
}

ZEST_CASE(LowLimitEnforced) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(1);
    auto r = f.test_low_dispatch(2);
    ZEXPECT(r.dispatched == 1u);
    ZEXPECT(r.busy_at_dispatch == 1u);
    // Destroying the unconsumed waiters released their claims.
    ZEXPECT(f.busy_count() == 0u);
}

ZEST_CASE(ReleaseDispatchesPending) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_release_dispatches();
    ZEXPECT(!r.pending_before);
    ZEXPECT(r.dispatched_after);
}

ZEST_CASE(SlotRAIIRelease) {
    WorkerPoolFixture f;
    f.add_stateless();
    ZEXPECT(f.test_slot_raii(0));
}

ZEST_CASE(ConcurrencyLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(1);

    int concurrent = 0;
    int max_concurrent = 0;
    bool done = false;

    f.run([&]() -> kota::task<> {
        kota::task_group<> group;
        auto work = [&]() -> kota::task<> {
            auto idx = co_await f.acquire_slot(worker::Priority::Low);
            ++concurrent;
            if(concurrent > max_concurrent)
                max_concurrent = concurrent;
            co_await kota::sleep(10);
            --concurrent;
            f.release_slot(idx);
        };
        for(int i = 0; i < 3; ++i)
            group.spawn(work());
        co_await group.join();
        done = true;
    });
    ZEXPECT(done);
    ZEXPECT(max_concurrent == 1);
}

ZEST_CASE(PriorityOrdering) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);

    std::size_t order = 0;
    std::size_t high_order = 0;
    std::size_t low_order = 0;
    bool done = false;

    f.run([&]() -> kota::task<> {
        kota::task_group<> group;

        auto initial = co_await f.acquire_slot(worker::Priority::Low);

        auto high_work = [&]() -> kota::task<> {
            auto idx = co_await f.acquire_slot(worker::Priority::High);
            high_order = ++order;
            f.release_slot(idx);
        };
        auto low_work = [&]() -> kota::task<> {
            auto idx = co_await f.acquire_slot(worker::Priority::Low);
            low_order = ++order;
            f.release_slot(idx);
        };

        group.spawn(high_work());
        group.spawn(low_work());

        f.release_slot(initial);
        co_await group.join();
        done = true;
    });
    ZEXPECT(done);
    ZEXPECT(high_order < low_order);
}

ZEST_CASE(SingleWorkerShared) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    ZEXPECT(f.max_low_limit() == 1u);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto low = co_await f.acquire_slot(worker::Priority::Low);
        ZEXPECT(f.is_busy(low));
        f.release_slot(low);

        auto high = co_await f.acquire_slot(worker::Priority::High);
        ZEXPECT(f.is_busy(high));
        f.release_slot(high);
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(PendingCleanupOnDestroy) {
    WorkerPoolFixture f;
    ZEXPECT(f.test_pending_cleanup_high());
    ZEXPECT(f.test_pending_cleanup_low());
}

ZEST_CASE(DispatchClearsQueue) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_dispatch_clears_queue();
    ZEXPECT(r.dispatched);
    ZEXPECT(r.queue_cleared);
    ZEXPECT(r.queue_ptr_nulled);
}

ZEST_CASE(FreshAcquireRespectsQueue) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);

    bool acquired = false;
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto queued = f.enqueue_waiter(worker::Priority::High);

        kota::task_group<> group;
        auto fresh = [&]() -> kota::task<> {
            auto idx = co_await f.acquire_slot(worker::Priority::High);
            acquired = true;
            f.release_slot(idx);
        };
        group.spawn(fresh());
        co_await kota::sleep(10);

        // The fresh acquire lined up behind the queued waiter instead of
        // claiming the idle worker over its head.
        ZEXPECT(!acquired);
        ZEXPECT(f.high_queue_size() == 2u);

        // The idle worker goes to the queued waiter first.
        f.dispatch_pending();
        ZEXPECT(!acquired);
        ZEXPECT(f.is_busy(0));

        // Destroying the unconsumed claim releases the slot to the fresh
        // waiter.
        queued.reset();
        co_await group.join();
        ZEXPECT(acquired);
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(EffectiveLimitClamped) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);
    // Capacity 3 caps the effective allowance regardless of low_limit; no
    // standing reservation — foreground bursts reclaim slots through the
    // foreground cap and the deficit cancel instead.
    ZEXPECT(f.max_low_limit() == 3u);
    ZEXPECT(f.effective_low_limit() == 3u);
}

ZEST_CASE(LowCapCountsLive) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    f.mark_dead(1);

    // A slot awaiting respawn is future capacity, not schedulable now: the
    // ceiling must track the two live workers, not the three allocated
    // slots, for the whole respawn window.
    ZEXPECT(f.max_low_limit() == 2u);
    ZEXPECT(f.effective_low_limit() == 2u);
}

ZEST_CASE(LowCeilingIsConfigured) {
    WorkerPoolFixture f;
    for(int i = 0; i < 4; i += 1) {
        f.add_stateless();
    }
    f.set_low_limit(8);
    f.set_stateless_count(2);

    // Workers past the configured count serve interactive work only.
    ZEXPECT(f.max_low_limit() == 2u);
    ZEXPECT(f.effective_low_limit() == 2u);
}

ZEST_CASE(LowCapSkipsRetiring) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    f.set_retiring(1);

    // A retiring slot is alive but takes no new work, so it must not
    // count toward the schedulable ceiling.
    ZEXPECT(f.max_low_limit() == 2u);
    ZEXPECT(f.effective_low_limit() == 2u);
}

ZEST_CASE(ForegroundCapClampsBudget) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    // Idle: the full schedulable capacity, no standing reservation.
    ZEXPECT(f.pool.effective_low_limit() == 4u);

    // Active: one background task, whatever the pool's size.
    f.pool.foreground_pulse();
    ZEXPECT(f.pool.effective_low_limit() == 1u);
}

ZEST_CASE(ForegroundCancelsExcess) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.set_low_limit(4);
    std::vector<std::shared_ptr<kota::cancellation_source>> sources;
    for(std::size_t i = 0; i < 4; i += 1) {
        f.set_claim_epoch(i, i + 1);
        sources.push_back(f.arm_cancel_source(i));
    }

    // The rising edge cancels exactly the over-cap excess (4 busy − cap 1),
    // newest claim first, cooperatively (slots stay alive).
    f.pool.foreground_pulse();

    ZEXPECT((f.cancel_asked(1) && f.cancel_asked(2) && f.cancel_asked(3)));
    ZEXPECT(sources[3]->cancelled());
    ZEXPECT(!f.cancel_asked(0));
    ZEXPECT(!sources[0]->cancelled());
    ZEXPECT(f.state(3) == WorkerPoolFixture::SlotState::Alive);
}

ZEST_CASE(ForegroundHoldExpires) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    f.pool.foreground_pulse();
    ZEXPECT(f.pool.effective_low_limit() == 1u);

    // Still inside the hold window: the tick keeps the clamp.
    f.tick_foreground();
    ZEXPECT(f.pool.effective_low_limit() == 1u);

    // Rewind the last activity past the hold: the tick reopens the budget.
    f.rewind_fg_activity();
    f.tick_foreground();
    ZEXPECT(f.pool.effective_low_limit() == 2u);
}

ZEST_CASE(GraceKillReclaims) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.arm_cancel_source(0);
    f.cancel_low(1);

    // Within the grace window the worker lives on.
    f.tick_cancel_grace();
    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Alive);

    // Past it, the stuck process is reclaimed like a memory preemption:
    // no crash accounting, immediate respawn.
    f.expire_cancel_grace(0);
    f.tick_cancel_grace();
    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
}

ZEST_CASE(CancelSkipsUnarmedSlots) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.set_claim_epoch(0, 1);
    f.set_claim_epoch(1, 2);
    // Slot 1 was claimed but its sender has not resumed: no source yet.
    auto armed = f.arm_cancel_source(0);

    f.cancel_low(2);

    // Only the armed slot may be stamped — stamping the un-asked one would
    // get a request that never saw a cancel grace-killed.
    ZEXPECT(f.cancel_asked(0));
    ZEXPECT(armed->cancelled());
    ZEXPECT(!f.cancel_asked(1));
}

ZEST_CASE(CancelSkipsAskedSlots) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.set_claim_epoch(0, 1);
    f.set_claim_epoch(1, 2);
    f.arm_cancel_source(0);
    f.arm_cancel_source(1);

    f.cancel_low(1);
    f.cancel_low(1);

    // The second ask must move on to the older claim instead of re-asking.
    ZEXPECT((f.cancel_asked(0) && f.cancel_asked(1)));
}

ZEST_CASE(DeficitSurvivesUnarmedSweep) {
    WorkerPoolFixture f;
    for(int i = 0; i < 4; i += 1) {
        f.add_stateless(true, true, true);
    }
    f.set_low_limit(8);

    // The rising edge finds only unarmed claims: the sweep stamps nothing,
    // but the demand survives for the arming re-check to honor.
    f.pool.foreground_pulse();
    ZEXPECT((!f.cancel_asked(0) && !f.cancel_asked(1) && !f.cancel_asked(2) && !f.cancel_asked(3)));
    ZEXPECT(f.reclaim_deficit() == 3u);

    // Once the senders arm and the asks land, the deficit is covered.
    for(std::size_t i = 1; i < 4; i += 1) {
        f.arm_cancel_source(i);
    }
    f.cancel_low(f.reclaim_deficit());
    ZEXPECT((f.cancel_asked(1) && f.cancel_asked(2) && f.cancel_asked(3)));
    ZEXPECT(f.reclaim_deficit() == 0u);
}

ZEST_CASE(DeficitCountsQueuedHigh) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.set_low_limit(8);

    // Within budget and no High demand: nothing owed. A queued High with
    // no idle slot owes one low even though the budget alone allows it.
    ZEXPECT(f.reclaim_deficit() == 0u);
    f.queue_high();
    ZEXPECT(f.reclaim_deficit() == 1u);

    // An ask already in flight covers the queued High.
    f.arm_cancel_source(0);
    f.cancel_low(1);
    ZEXPECT(f.reclaim_deficit() == 0u);
}

ZEST_CASE(QueuedLowHonorsCancel) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(0);

    kota::cancellation_source source;
    std::size_t acquired = 0;
    f.run([&]() -> kota::task<> {
        kota::task_group<> group;
        auto waiter = [&]() -> kota::task<> {
            acquired = co_await f.acquire_slot(worker::Priority::Low, source.token());
        };
        group.spawn(waiter());
        // Parked behind a zero budget, it leaves the queue once its round
        // gives up, instead of waiting for the budget to come back.
        ZEXPECT(f.low_queue_size() == 1u);
        source.cancel();
        co_await group.join();
    });
    ZEXPECT(acquired == SIZE_MAX);
    ZEXPECT(f.low_queue_size() == 0u);
    ZEXPECT(f.busy_count() == 0u);
}

ZEST_CASE(CancelAfterClaimReleases) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    kota::cancellation_source source;
    std::size_t acquired = 0;
    f.run([&]() -> kota::task<> {
        kota::task_group<> group;
        auto waiter = [&]() -> kota::task<> {
            acquired = co_await f.acquire_slot(worker::Priority::Low, source.token());
        };
        group.spawn(waiter());
        ZEXPECT(f.low_queue_size() == 1u);
        // The freed slot is claimed for the waiter, whose round gives up
        // before it resumes: the claim must come back.
        f.release_slot(0);
        ZEXPECT(f.is_busy(0));
        source.cancel();
        co_await group.join();
    });
    ZEXPECT(acquired == SIZE_MAX);
    ZEXPECT(f.busy_count() == 0u);
}

ZEST_CASE(StarvedHighGrowsPool) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, false);
    f.add_stateless(true, true, false);
    f.pool.foreground_pulse();

    // Interactive work waiting on a full pool asks for a worker even while
    // the user is active: the user is the one waiting.
    f.tick_scaling(1.0);
    ZEXPECT(f.saturated_cycles() == 0u);
    f.queue_high();
    f.tick_scaling(1.0);
    ZEXPECT(f.saturated_cycles() == 1u);
}

ZEST_CASE(LowGrowthHasCeiling) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, true);
    f.set_low_limit(2);
    f.set_stateless_count(2);
    auto waiter = f.enqueue_waiter(worker::Priority::Low);

    // Background work queued behind a full budget grows the pool only back
    // up to the configured count, and not while the user is active.
    f.tick_scaling(1.0);
    ZEXPECT(f.saturated_cycles() == 0u);
    f.set_stateless_count(3);
    f.tick_scaling(1.0);
    ZEXPECT(f.saturated_cycles() == 1u);
    f.pool.foreground_pulse();
    f.tick_scaling(1.0);
    ZEXPECT(f.saturated_cycles() == 0u);
}

};  // ZEST_SUITE(WorkerPoolScheduling)

ZEST_SUITE(WorkerPoolCrash) {

ZEST_CASE(StatelessCleanup) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    ZEXPECT(f.alive_count() == 2u);
    ZEXPECT(f.busy_count() == 1u);

    f.simulate_crash(0, false);

    ZEXPECT(f.alive_count() == 1u);
    ZEXPECT(f.busy_count() == 0u);
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
        ZEXPECT(llvm::is_contained(report.lost_documents, path_id));
    }

    // Other worker's documents are preserved
    for(auto id: {100u, 200u, 300u}) {
        if(!llvm::is_contained(expected, id))
            ZEXPECT(f.has_owner(id));
    }
}

ZEST_CASE(CrashReportsAnomaly) {
    /// Production trigger for the WorkerCrash anomaly: process_crash is the
    /// exact site the pool reports from.
    WorkerPoolFixture f;
    f.add_stateless(true, false);

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
    f.add_stateless(true, false);

    f.simulate_crash(0, false, 0, 11);

    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(f.crash_reports[0].exit_signal == 11);
    ZEXPECT(f.crash_reports[0].exit_code == 0);
    ZEXPECT(!f.crash_reports[0].stateful);
}

ZEST_CASE(CrashInfoExitCode) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);

    f.simulate_crash(0, false, 42, 0);

    ZASSERT(f.crash_reports.size() == 1u);
    ZEXPECT(f.crash_reports[0].exit_code == 42);
    ZEXPECT(f.crash_reports[0].exit_signal == 0);
}

ZEST_CASE(WillRestartWithinBudget) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
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
    f.add_stateless(true, false);
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
    f.add_stateless(true, false);
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
    f.add_stateless(true, false);
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
    auto first = f.dispatch(0, true, "clice/worker/compile /a.cpp");
    auto second = f.dispatch(0, true, "clice/worker/compile /b.cpp");
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
    death.culprit = "clice/worker/compile /a.cpp";

    using namespace worker::dispatch_errc;
    auto own = WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp");
    ZEXPECT(own.code == worker_crashed);
    ZEXPECT(own.message == death.cause);
    ZEXPECT(worker::death_of(own) == "sf:0:1");
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /b.cpp").code ==
            worker_lost);

    death.culprit.clear();
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp").code ==
            worker_died);

    // Among several requests a nameless death accuses none of them, and a
    // memory reclaim accuses its request of nothing either.
    death.in_flight = 2;
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp").code ==
            worker_lost);
    death.in_flight = 1;
    death.reclaimed = true;
    ZEXPECT(WorkerPoolFixture::death_error(death, "clice/worker/compile /a.cpp").code ==
            worker_lost);
}

ZEST_CASE(DeathCountsCompany) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.add_stateful(true);
    auto alone = f.dispatch(0, true, "clice/worker/compile /a.cpp");
    auto first = f.dispatch(1, true, "clice/worker/compile /b.cpp");
    auto second = f.dispatch(1, true, "clice/worker/compile /c.cpp");
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
    // land on the corpse: the death itself frees the document.
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
    // A death of another request's doing, or one that named no request,
    // is resent once.
    for(auto code: {worker_lost, worker_died}) {
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

ZEST_CASE(DeadlineKillsAndNames) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.add_stateful(true);
    f.set_deadlines(std::chrono::seconds(10), std::chrono::seconds(1));
    // A build outlives the query deadline; a query does not.
    auto build = f.dispatch(0, true, "clice/worker/compile /a.cpp", std::chrono::seconds(2), true);
    auto hung = f.dispatch(1, true, "clice/worker/query:Hover /b.cpp", std::chrono::seconds(2));

    f.add_stateless(true, true);
    auto run = f.dispatch(0, false, "clice/worker/tuRun /c.cpp", std::chrono::seconds(20), true);

    // A query queued behind a build in time is no hang of its own.
    f.add_stateful(true);
    auto compiling =
        f.dispatch(2, true, "clice/worker/compile /d.cpp", std::chrono::seconds(5), true);
    auto queued = f.dispatch(2, true, "clice/worker/query:Hover /d.cpp", std::chrono::seconds(5));

    f.tick_deadlines();
    ZEXPECT(f.state(2, true) == WorkerPoolFixture::SlotState::Alive);
    ZEXPECT(f.state(0, true) == WorkerPoolFixture::SlotState::Alive);
    ZEXPECT(f.state(1, true) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.death(1, true)->culprit == "clice/worker/query:Hover /b.cpp");
    ZEXPECT(f.death(1, true)->cause.contains("1 seconds"));
    ZEXPECT(f.state(0, false) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.death(0, false)->culprit == "clice/worker/tuRun /c.cpp");
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
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    ZEXPECT(f.busy_count() == 1u);
    f.mark_dead(0);

    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.generation(0) == 1u);
    ZEXPECT(f.busy_count() == 0u);
    // The dying slot still counts as future capacity (its verdict is
    // pending), but is not schedulable.
    ZEXPECT(f.capacity() == 2u);
    ZEXPECT(f.pick_idle() == 1u);

    // Idempotent: a second mark (e.g. the monitor's) is a no-op.
    f.mark_dead(0);
    ZEXPECT(f.generation(0) == 1u);
}

ZEST_CASE(ReleaseIdempotent) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    ZEXPECT(f.busy_count() == 1u);
    f.release_slot(0);
    ZEXPECT(f.busy_count() == 0u);
    f.release_slot(0);
    ZEXPECT(f.busy_count() == 0u);
}

ZEST_CASE(CrashThenRelease) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    f.simulate_crash(0, false);
    ZEXPECT(f.busy_count() == 0u);

    // StatelessSlot destructor would call release_slot — must not underflow
    f.release_slot(0);
    ZEXPECT(f.busy_count() == 0u);
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
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    ZEXPECT(f.alive_count() == 2u);
    ZEXPECT(f.busy_count() == 0u);

    f.simulate_crash(0, false);

    ZEXPECT(f.alive_count() == 1u);
    ZEXPECT(f.busy_count() == 0u);
    ZEXPECT(!f.worker_alive(0));
}

ZEST_CASE(RapidCrashSequence) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(8);

    f.simulate_crash(0, false);
    f.simulate_crash(1, false);
    f.simulate_crash(2, false);

    ZEXPECT(f.alive_count() == 0u);
    ZEXPECT(f.busy_count() == 0u);
    ZEXPECT(f.crash_reports.size() == 3u);
}

ZEST_CASE(DeadPoolReturnsError) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_low_limit(1);
    f.set_max_crash_streak(0);

    f.simulate_crash(0, false);
    ZEXPECT(f.alive_count() == 0u);
    ZEXPECT(f.capacity() == 0u);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        ZEXPECT(idx == SIZE_MAX);
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(DeadPoolDrainsPending) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    f.set_max_crash_streak(0);

    auto r = f.test_dead_pool_drain();
    ZEXPECT(r.drained);
    ZEXPECT(r.assigned_worker == SIZE_MAX);
    ZEXPECT(f.high_queue_size() == 0u);
}

ZEST_CASE(SlotGenGuard) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    ZEXPECT(f.test_slot_gen_guard());
}

ZEST_CASE(PendingClaimGenGuard) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    ZEXPECT(f.test_pending_gen_guard());
}

ZEST_CASE(CrashBudgetBoundary) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
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
    // A request landing in the restart window never reaches a worker: the
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
    // A slot marked dead but without a verdict still counts as future
    // capacity: waiters must keep waiting for the respawn instead of
    // failing out.
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    f.mark_dead(0);
    ZEXPECT(f.test_low_dispatch(1).dispatched == 0u);
    ZEXPECT(f.low_queue_size() == 0u);  // helper destroys its waiters
}

ZEST_CASE(MaxLowLimitShrinks) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_low_limit(2);
    f.set_max_crash_streak(0);

    f.simulate_crash(0, false);

    // Capacity dropped to 2, so the ceiling tracks it.
    ZEXPECT(f.max_low_limit() == 2u);
    ZEXPECT(f.effective_low_limit() == 2u);
}

};  // ZEST_SUITE(WorkerPoolCrash)

ZEST_SUITE(WorkerPoolMemory) {

ZEST_CASE(PreemptKillsLowWorkers) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);   // busy low
    f.add_stateless(true, true, false);  // busy high — must survive
    f.set_low_limit(2);
    auto death = f.death(0, false);

    f.preempt(2);

    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.preempted(0));
    // The killed request may be what exhausted memory: its sender hears a
    // loss that spends its retries, not a free cancel.
    ZEXPECT(death->reclaimed);
    ZEXPECT(f.state(1) == WorkerPoolFixture::SlotState::Alive);
    ZEXPECT(f.is_busy(1));
    ZEXPECT(f.low_busy() == 0u);
    // Preemption is not a crash: no report, no crash accounting.
    ZEXPECT(f.crash_reports.empty());
    ZEXPECT(f.crash_streak(0) == 0u);
}

ZEST_CASE(PreemptCreditsHealthyRun) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.set_crash_streak(0, false, 2);
    f.set_uptime(0, false, std::chrono::milliseconds(60'000));

    f.preempt(1);

    // The healthy interval before the preemption clears the stale streak,
    // exactly as the next crash's accounting would have.
    ZEXPECT(f.crash_streak(0) == 0u);
}

ZEST_CASE(PreemptKeepsYoungStreak) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.set_crash_streak(0, false, 2);
    f.set_uptime(0, false, std::chrono::milliseconds(0));

    f.preempt(1);

    // A slot that has not yet earned the healthy credit keeps its streak:
    // preemption neither punishes nor forgives.
    ZEXPECT(f.crash_streak(0) == 2u);
}

ZEST_CASE(SevereMemoryTick) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    f.tick_memory(0.05);

    // Zero the allowance and kill the low work. Zero, not one: any
    // allowance left would let the preemption's own dispatch kick admit a
    // fresh compile into the pressure being relieved.
    ZEXPECT(f.low_limit() == 0u);
    ZEXPECT(f.low_busy() == 0u);
    ZEXPECT(f.state(0) == WorkerPoolFixture::SlotState::Dying);
    ZEXPECT(f.state(1) == WorkerPoolFixture::SlotState::Dying);
}

ZEST_CASE(PressureDecrementsLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);

    f.tick_memory(0.15);
    ZEXPECT(f.low_limit() == 1u);

    // Never below one while memory is tight but not exhausted.
    f.tick_memory(0.15);
    ZEXPECT(f.low_limit() == 1u);
}

ZEST_CASE(ZeroBudgetLifts) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);

    // A zeroed budget comes back at the first tick that is not severe, in
    // the pressure band and above it alike.
    f.tick_memory(0.05);
    ZEXPECT(f.low_limit() == 0u);
    f.tick_memory(0.15);
    ZEXPECT(f.low_limit() == 1u);

    f.tick_memory(0.05);
    ZEXPECT(f.low_limit() == 0u);
    f.tick_memory(0.30);
    ZEXPECT(f.low_limit() == 1u);
}

ZEST_CASE(GrowthNeedsFullBudget) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);

    // One of two budgeted slots busy: the budget is not what limits the
    // work, so it does not grow.
    f.tick_memory(0.50);
    ZEXPECT(f.low_limit() == 2u);

    f.add_stateless(true, true, true);
    f.tick_memory(0.50);
    ZEXPECT(f.low_limit() == 3u);
}

ZEST_CASE(GrowthUnderForeground) {
    WorkerPoolFixture f;
    for(int i = 0; i < 4; i += 1) {
        f.add_stateless();
    }
    f.set_low_limit(0);
    f.pool.foreground_pulse();
    f.add_stateless(true, true, true);

    // While the user is active the budget recovers, but only one step past
    // the foreground limit: its release admits no untested burst.
    for(int i = 0; i < 5; i += 1) {
        f.tick_memory(0.50);
    }
    ZEXPECT(f.low_limit() == 2u);
    ZEXPECT(f.effective_low_limit() == 1u);
}

ZEST_CASE(CgroupFilesFollowLibuv) {
    auto v2 = WorkerPoolFixture::find_cgroup_memory("0::/user.slice/clice.scope\n");
    ZEXPECT(v2.usage == "/sys/fs/cgroup/user.slice/clice.scope/memory.current");
    ZEXPECT(v2.stat == "/sys/fs/cgroup/user.slice/clice.scope/memory.stat");
    ZEXPECT(v2.cache_key == "inactive_file");

    // A v1 memory cgroup missing from the hierarchy, or no memory line of
    // its own, falls back to the hierarchy's root.
    auto v1 = WorkerPoolFixture::find_cgroup_memory(
        "12:cpu,cpuacct:/a\n4:memory:/clice-missing\n1:name=systemd:/b\n");
    ZEXPECT(v1.usage == "/sys/fs/cgroup/memory/memory.usage_in_bytes");
    ZEXPECT(v1.stat == "/sys/fs/cgroup/memory/memory.stat");
    ZEXPECT(v1.cache_key == "total_inactive_file");

    auto grouped = WorkerPoolFixture::find_cgroup_memory("4:cpu,memory:/a\n");
    ZEXPECT(grouped.usage == "/sys/fs/cgroup/memory/memory.usage_in_bytes");
}

ZEST_CASE(CacheCountsAvailable) {
    constexpr std::uint64_t gib = 1ull << 30;
    TempDir tmp;
    tmp.touch("memory.current", std::to_string(3 * gib));
    tmp.touch("memory.stat", std::format("anon {}\ninactive_file {}\n", 2 * gib, gib));
    auto usage = tmp.path("memory.current");
    auto stat = tmp.path("memory.stat");

    // A 4 GiB limit with 3 GiB used, 1 GiB of it reclaimable cache.
    auto available = WorkerPoolFixture::cgroup_available(usage, stat, 4 * gib);
    ZASSERT(available.has_value());
    ZEXPECT(*available == 2 * gib);

    // Usage past the limit: the cache does not make an exhausted cgroup
    // look healthy.
    tmp.touch("memory.current", std::to_string(5 * gib));
    available = WorkerPoolFixture::cgroup_available(usage, stat, 4 * gib);
    ZASSERT(available.has_value());
    ZEXPECT(*available == 0u);

    ZEXPECT(!WorkerPoolFixture::cgroup_available("", "", 4 * gib).has_value());
}

ZEST_CASE(BudgetFollowsRetirement) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    // A budget above the ceiling is brought down to it before the step.
    f.tick_memory(0.15);
    ZEXPECT(f.low_limit() == 1u);
}

};  // ZEST_SUITE(WorkerPoolMemory)

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

ZEST_CASE(StopIsPrompt) {
    // Regression guard: stop() must cancel the monitor loop's 3s poll
    // sleep instead of waiting it out.
    WorkerPoolFixture f;
    std::chrono::milliseconds elapsed{};
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));
        auto begin = std::chrono::steady_clock::now();
        co_await f.stop();
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
    });
    ZEXPECT(elapsed.count() < 1500);
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
            [&](const kota::ipc::Error&) { blames += 1; });
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

        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        auto result = co_await f.pool.send_stateless(params, worker::Priority::Low);
        ZEXPECT(result);
        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(AdvisoryCancelCooperates) {
    // s#1: an advisory-token fire mid-build cancels the request on the
    // wire. The sender keeps awaiting the real reply — the slot
    // frees only once the worker is actually idle — and the result is
    // classified as a cancellation, with the worker healthy afterwards.
    TempDir tmp;
    std::string big;
    for(int i = 0; i < 200000; i += 1) {
        big += "int v_" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    tmp.touch("big.cpp", big);
    auto big_src = tmp.path("big.cpp");
    tmp.touch("small.cpp", "int y = 2;\n");
    auto small_src = tmp.path("small.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(1, 0));

        worker::TURunParams params;
        params.index = true;
        params.file = big_src;
        params.directory = "/tmp";
        params.arguments = make_args(big_src);

        kota::cancellation_source advisory;
        bool cancelled = false;
        auto sender = [&]() -> kota::task<> {
            auto result =
                co_await f.pool.send_stateless(params, worker::Priority::Low, advisory.token());
            cancelled =
                !result.has_value() && result.error().code == worker::dispatch_errc::cancelled;
        };
        auto driver = [&]() -> kota::task<> {
            co_await kota::sleep(300);
            advisory.cancel();
            co_return;
        };
        co_await kota::when_all(sender(), driver());
        ZEXPECT(cancelled);

        // The worker replied and survived: the same slot serves the next
        // request.
        worker::TURunParams follow;
        follow.index = true;
        follow.file = small_src;
        follow.directory = "/tmp";
        follow.arguments = make_args(small_src);
        auto result = co_await f.pool.send_stateless(follow, worker::Priority::Low);
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

        // Wait for respawn: PID must change (not just alive, which is
        // true before the kill is processed).
        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(f.worker_alive(0) && f.worker_pid(0) != pid)
                break;
        }
        ZEXPECT(f.worker_alive(0));
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
        for(int i = 0; i < 50 && !f.slot_dead(0); ++i) {
            co_await kota::sleep(100);
        }
        ZEXPECT(f.slot_dead(0));

        // ...and is revived with a fresh budget after the cooldown: the
        // pool never stays at zero workers forever.
        for(int i = 0; i < 100 && !f.worker_alive(0); ++i) {
            co_await kota::sleep(100);
        }
        ZEXPECT(f.worker_alive(0));
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
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        ZEXPECT(worker_score(idx) > own);

        // Against a limit so large the master's share rounds to nothing, a
        // worker holding work ranks just above the master; an idle one
        // stays level with it.
        constexpr std::uint64_t huge = 1ull << 50;
        f.tick_oom_scores(huge);
        ZEXPECT(worker_score(idx) == std::min(own + 2, 1000));
        ZEXPECT(worker_score(1 - idx) == own);

        // Released, it drops back at once.
        f.release_slot(idx);
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
        for(int i = 0; i < 50 && !f.slot_dead(0); ++i) {
            co_await kota::sleep(100);
        }
        ZASSERT(f.slot_dead(0));

        // Scale-up pressure revives the dead slot instead of appending a
        // third worker next to it.
        ZASSERT(f.scale_up());
        ZEXPECT(f.stateless_count() == 2u);
        for(int i = 0; i < 50 && !f.worker_alive(0); ++i) {
            co_await kota::sleep(100);
        }
        ZEXPECT(f.worker_alive(0));
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
        f.retire_idle();
        for(int i = 0; i < 50 && !f.slot_retired(1); i += 1) {
            co_await kota::sleep(100);
        }
        ZASSERT(f.slot_retired(1));

        // The retired slot is refilled instead of a third appended: a
        // retire/scale-up cycle must not grow the slot table.
        ZASSERT(f.scale_up());
        ZEXPECT(f.stateless_count() == 2u);
        for(int i = 0; i < 50 && !f.worker_alive(1); i += 1) {
            co_await kota::sleep(100);
        }
        ZEXPECT(f.worker_alive(1));

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

        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(!f.crash_reports.empty())
                break;
        }

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

        // Kill both workers, then send without yielding: the claim lands on
        // a dead-but-not-yet-reaped slot, and the pool must surface the
        // death instead of retrying internally — worker_died, since an
        // outside kill names no request.
        f.kill_worker(0);
        f.kill_worker(1);

        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        auto result = co_await f.pool.send_stateless(params, worker::Priority::Low);
        ZASSERT(!result.has_value());
        ZEXPECT(result.error().code == worker::dispatch_errc::worker_died);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(PreemptReportsLost) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        worker::protocol::integer code = 0;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(params, worker::Priority::Low);
            if(!result.has_value())
                code = result.error().code;
        };
        group.spawn(sender());
        // Wait until the claim is visible, then preempt within the same
        // loop turn: with no suspension in between, the worker's response
        // cannot be processed first, so the preemption deterministically
        // catches the request in flight.
        while(f.low_busy() == 0)
            co_await kota::sleep(1);
        f.preempt(1);
        co_await group.join();
        ZEXPECT(code == worker::dispatch_errc::worker_lost);

        // Preemption is not a crash: no report, and the worker comes back
        // with an untouched budget.
        ZEXPECT(f.crash_reports.empty());
        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(f.worker_alive(0))
                break;
        }
        ZEXPECT(f.worker_alive(0));
        ZEXPECT(f.crash_streak(0) == 0u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(ReclaimBeatsCancel) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(2, 0));

        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        kota::cancellation_source source;
        worker::protocol::integer code = 0;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result =
                co_await f.pool.send_stateless(params, worker::Priority::Low, source.token());
            if(!result.has_value())
                code = result.error().code;
        };
        group.spawn(sender());
        while(f.low_busy() == 0)
            co_await kota::sleep(1);
        // A memory kill landing before the cancelled request stopped is
        // reported as such: the request may be what exhausted memory, and a
        // free requeue would repeat the kill.
        source.cancel();
        f.preempt(1);
        co_await group.join();
        ZEXPECT(code == worker::dispatch_errc::worker_lost);

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

        worker::TURunParams params;
        params.index = true;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        worker::protocol::integer code = 0;
        kota::task_group<> group;
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(params, worker::Priority::Low);
            if(!result.has_value())
                code = result.error().code;
        };
        group.spawn(sender());
        while(f.low_busy() == 0)
            co_await kota::sleep(1);

        // A cooperative cancel whose victim then dies on its own inside the
        // grace window: the request still classifies as cancelled, but the
        // slot must be retired with it — released as Alive it would take
        // the next dispatch before the monitor observes the exit. The
        // generation bump is the retirement evidence that survives an
        // instant respawn.
        auto gen = f.generation(0);
        f.cancel_low(1);
        f.kill_worker(0);
        co_await group.join();
        ZEXPECT(code == worker::dispatch_errc::cancelled);
        ZEXPECT(f.generation(0) != gen);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

ZEST_CASE(ScaleUpKeepsLimit) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        ZASSERT(f.start(3, 0));
        // Simulate pressure having reduced the allowance below the ceiling.
        f.set_low_limit(1);

        ZASSERT(f.scale_up());

        // The new worker adds exactly one to the allowance; it must not
        // reset the reduction back to the ceiling, which stays the
        // configured three: the fourth serves interactive work.
        ZEXPECT(f.max_low_limit() == 3u);
        ZEXPECT(f.low_limit() == 2u);

        co_await f.stop();
        done = true;
    });
    ZEXPECT(done);
}

};  // ZEST_SUITE(WorkerPoolIntegration)

}  // namespace

}  // namespace clice::testing
