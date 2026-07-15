#include <chrono>

#include "test/test.h"
#include "server/protocol/worker.h"
#include "server/worker/worker_pool.h"
#include "server/worker_test_helpers.h"
#include "support/anomaly.h"

#include "kota/async/async.h"

namespace clice::testing {

/// Test fixture with friend access to WorkerPool internals.
struct WorkerPoolFixture {
    using SlotState = WorkerPool::SlotState;

    kota::event_loop loop;
    WorkerPool pool;

    std::vector<WorkerCrashInfo> crash_reports;

    WorkerPoolFixture() : pool(loop) {
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

    std::size_t assign_expendable(std::uint32_t path_id) {
        return pool.assign_expendable(path_id);
    }

    void remove_owner(std::uint32_t path_id) {
        pool.remove_owner(path_id);
    }

    void clear_owner(std::size_t idx) {
        pool.clear_owner(idx);
    }

    std::size_t pick_idle() {
        return pool.pick_idle_stateless();
    }

    void apply_backoff() {
        pool.apply_crash_backoff();
    }

    void release_slot(std::size_t idx) {
        pool.release_stateless_slot(idx);
    }

    kota::task<std::size_t> acquire_slot(worker::Priority p) {
        return pool.acquire_stateless_slot(p);
    }

    bool simulate_crash(std::size_t index, bool stateful, int exit_code = 0, int exit_signal = 9) {
        return pool.process_crash(index, stateful, exit_code, exit_signal);
    }

    void mark_dead(std::size_t idx, bool stateful = false) {
        pool.mark_worker_dead(idx, stateful, false);
    }

    void set_retiring(std::size_t idx) {
        pool.stateless_workers[idx].retiring = true;
    }

    void set_suspect_inflight(std::size_t idx, bool stateful, unsigned n) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].suspect_inflight = n;
    }

    void set_revive_after(std::chrono::milliseconds cooldown) {
        pool.options.revive_after = cooldown;
    }

    void set_max_stateless(std::size_t n) {
        pool.options.max_stateless = n;
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

    unsigned suspect_inflight(std::size_t idx, bool stateful) const {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        return workers[idx].suspect_inflight;
    }

    kota::task<> stop() {
        return pool.stop();
    }

    template <typename F>
    void run(F&& coro_factory) {
        loop.schedule(coro_factory());
        loop.run();
    }

    void kill_worker(std::size_t idx) {
        pool.stateless_workers[idx].proc.kill(9);
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

    std::size_t w_max() const {
        return pool.w_max;
    }

    void set_w_max(std::size_t value) {
        pool.w_max = value;
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

    unsigned get_backoff_cooldown() const {
        return pool.backoff_cooldown;
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

TEST_SUITE(WorkerPoolStateful) {

TEST_CASE(PickLeastLoaded) {
    WorkerPoolFixture f;
    f.add_stateful(true, 5);
    f.add_stateful(true, 2);
    f.add_stateful(true, 8);
    EXPECT_EQ(f.pick_least_loaded(), 1u);
}

TEST_CASE(SkipDeadWorkers) {
    WorkerPoolFixture f;
    f.add_stateful(false, 0);
    f.add_stateful(true, 5);
    f.add_stateful(true, 3);
    EXPECT_EQ(f.pick_least_loaded(), 2u);
}

TEST_CASE(ProbeWorkerNotPicked) {
    WorkerPoolFixture f;
    f.add_stateful(true, 5);
    f.add_stateful(true, 0);
    f.set_suspect_inflight(1, true, 1);

    // The idle worker hosts an in-flight quarantine probe — a known crash
    // risk. A new document goes to the busier worker instead.
    EXPECT_EQ(f.pick_least_loaded(), 0u);

    // With no alternative, the probe worker still serves.
    f.mark_dead(0, true);
    EXPECT_EQ(f.pick_least_loaded(), 1u);
}

TEST_CASE(StaleEvictionIgnored) {
    WorkerPoolFixture f;
    f.add_stateful();
    f.add_stateful();
    auto idx = f.assign_worker(7);

    // An eviction from a worker that lost ownership (probe reassignment
    // left it a stale copy) must not unseat the current owner.
    EXPECT_FALSE(f.pool.remove_owner_from(7, idx + 1));
    EXPECT_TRUE(f.has_owner(7));

    EXPECT_TRUE(f.pool.remove_owner_from(7, idx));
    EXPECT_FALSE(f.has_owner(7));
    EXPECT_EQ(f.stateful_owned(idx), 0u);
}

TEST_CASE(AllDeadNoAssignment) {
    WorkerPoolFixture f;
    f.add_stateful(false, 0);
    f.add_stateful(false, 0);
    EXPECT_EQ(f.pick_least_loaded(), SIZE_MAX);
    EXPECT_EQ(f.assign_worker(100), SIZE_MAX);
    // A failed assignment must not pin the document to a dead worker.
    EXPECT_FALSE(f.has_owner(100));
}

TEST_CASE(AssignNewPath) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto idx = f.assign_worker(100);
    EXPECT_EQ(f.stateful_owned(idx), 1u);
    EXPECT_TRUE(f.has_owner(100));
    EXPECT_EQ(f.owner_of(100), idx);
}

TEST_CASE(PathAffinity) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto a = f.assign_worker(100);
    auto b = f.assign_worker(100);
    EXPECT_EQ(a, b);
    EXPECT_EQ(f.stateful_owned(a), 1u);
}

TEST_CASE(LoadBalancing) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    auto a = f.assign_worker(100);
    auto b = f.assign_worker(200);
    EXPECT_NE(a, b);
}

TEST_CASE(RemoveOwner) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.assign_worker(100);
    EXPECT_EQ(f.stateful_owned(0), 1u);
    f.remove_owner(100);
    EXPECT_EQ(f.stateful_owned(0), 0u);
    EXPECT_FALSE(f.has_owner(100));
}

TEST_CASE(RemoveNonexistent) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.remove_owner(999);
}

TEST_CASE(ClearOwner) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    f.assign_worker(100);
    f.assign_worker(200);
    f.assign_worker(300);
    EXPECT_EQ(f.stateful_owned(0), 2u);
    f.clear_owner(0);
    EXPECT_EQ(f.stateful_owned(0), 0u);
    EXPECT_FALSE(f.has_owner(100));
    EXPECT_FALSE(f.has_owner(300));
    EXPECT_TRUE(f.has_owner(200));
}

TEST_CASE(RebalanceAfterClose) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);
    f.assign_worker(100);
    f.assign_worker(200);
    f.assign_worker(300);
    EXPECT_EQ(f.stateful_owned(0), 2u);
    EXPECT_EQ(f.stateful_owned(1), 1u);

    // Closing documents shrinks the owner table and load counts.
    f.remove_owner(100);
    f.remove_owner(300);
    EXPECT_EQ(f.stateful_owned(0), 0u);
    EXPECT_FALSE(f.has_owner(100));
    EXPECT_FALSE(f.has_owner(300));

    // New assignments go to the now least-loaded worker.
    EXPECT_EQ(f.assign_worker(400), 0u);
    EXPECT_EQ(f.stateful_owned(0), 1u);
}

};  // TEST_SUITE(WorkerPoolStateful)

TEST_SUITE(WorkerPoolScheduling) {

TEST_CASE(PickIdleBasic) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    EXPECT_EQ(f.pick_idle(), 1u);
}

TEST_CASE(PickIdleSkipsDead) {
    WorkerPoolFixture f;
    f.add_stateless(false, false);
    f.add_stateless(true, false);
    EXPECT_EQ(f.pick_idle(), 1u);
}

TEST_CASE(PickIdleNoneAvailable) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(false, false);
    EXPECT_EQ(f.pick_idle(), SIZE_MAX);
}

TEST_CASE(AcquireHighImmediate) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        EXPECT_TRUE(f.is_busy(idx));
        EXPECT_EQ(f.busy_count(), 1u);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(AcquireLowImmediate) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::Low);
        EXPECT_TRUE(f.is_busy(idx));
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(HighBypassesLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(0);
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        EXPECT_TRUE(f.is_busy(idx));
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(HighDispatchedFirst) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_priority_dispatch(0);
    EXPECT_TRUE(r.high_dispatched);
    EXPECT_FALSE(r.low_dispatched);
}

TEST_CASE(LowLimitEnforced) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(1);
    auto r = f.test_low_dispatch(2);
    EXPECT_EQ(r.dispatched, 1u);
    EXPECT_EQ(r.busy_at_dispatch, 1u);
    // Destroying the unconsumed waiters released their claims.
    EXPECT_EQ(f.busy_count(), 0u);
}

TEST_CASE(ReleaseDispatchesPending) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_release_dispatches();
    EXPECT_FALSE(r.pending_before);
    EXPECT_TRUE(r.dispatched_after);
}

TEST_CASE(SlotRAIIRelease) {
    WorkerPoolFixture f;
    f.add_stateless();
    EXPECT_TRUE(f.test_slot_raii(0));
}

TEST_CASE(ConcurrencyLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(1);

    int concurrent = 0;
    int max_concurrent = 0;
    bool done = false;

    f.run([&]() -> kota::task<> {
        kota::task_group<> group(f.loop);
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
    EXPECT_TRUE(done);
    EXPECT_EQ(max_concurrent, 1);
}

TEST_CASE(PriorityOrdering) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);

    std::size_t order = 0;
    std::size_t high_order = 0;
    std::size_t low_order = 0;
    bool done = false;

    f.run([&]() -> kota::task<> {
        kota::task_group<> group(f.loop);

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
    EXPECT_TRUE(done);
    EXPECT_TRUE(high_order < low_order);
}

TEST_CASE(SingleWorkerShared) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    EXPECT_EQ(f.max_low_limit(), 1u);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto low = co_await f.acquire_slot(worker::Priority::Low);
        EXPECT_TRUE(f.is_busy(low));
        f.release_slot(low);

        auto high = co_await f.acquire_slot(worker::Priority::High);
        EXPECT_TRUE(f.is_busy(high));
        f.release_slot(high);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(PendingCleanupOnDestroy) {
    WorkerPoolFixture f;
    EXPECT_TRUE(f.test_pending_cleanup_high());
    EXPECT_TRUE(f.test_pending_cleanup_low());
}

TEST_CASE(DispatchClearsQueue) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    auto r = f.test_dispatch_clears_queue();
    EXPECT_TRUE(r.dispatched);
    EXPECT_TRUE(r.queue_cleared);
    EXPECT_TRUE(r.queue_ptr_nulled);
}

TEST_CASE(FreshAcquireRespectsQueue) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);

    bool acquired = false;
    bool done = false;
    f.run([&]() -> kota::task<> {
        auto queued = f.enqueue_waiter(worker::Priority::High);

        kota::task_group<> group(f.loop);
        auto fresh = [&]() -> kota::task<> {
            auto idx = co_await f.acquire_slot(worker::Priority::High);
            acquired = true;
            f.release_slot(idx);
        };
        group.spawn(fresh());
        co_await kota::sleep(10);

        // The fresh acquire lined up behind the queued waiter instead of
        // claiming the idle worker over its head.
        EXPECT_FALSE(acquired);
        EXPECT_EQ(f.high_queue_size(), 2u);

        // The idle worker goes to the queued waiter first.
        f.dispatch_pending();
        EXPECT_FALSE(acquired);
        EXPECT_TRUE(f.is_busy(0));

        // Destroying the unconsumed claim releases the slot to the fresh
        // waiter.
        queued.reset();
        co_await group.join();
        EXPECT_TRUE(acquired);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(EffectiveLimitClamped) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);
    // Capacity 3 caps the effective allowance at 2 regardless of low_limit.
    EXPECT_EQ(f.max_low_limit(), 2u);
    EXPECT_EQ(f.effective_low_limit(), 2u);
}

TEST_CASE(LowCapCountsLive) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    f.mark_dead(1);

    // A slot awaiting respawn is future capacity, not schedulable now: the
    // reserve-one-for-high cap must not let low-priority work occupy both
    // live workers for the whole respawn window.
    EXPECT_EQ(f.max_low_limit(), 1u);
    EXPECT_EQ(f.effective_low_limit(), 1u);
}

TEST_CASE(LowCapSkipsRetiring) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(8);

    f.set_retiring(1);

    // A retiring slot is alive but takes no new work, so it must not
    // count toward the schedulable pool the cap reserves from.
    EXPECT_EQ(f.max_low_limit(), 1u);
    EXPECT_EQ(f.effective_low_limit(), 1u);
}

};  // TEST_SUITE(WorkerPoolScheduling)

TEST_SUITE(WorkerPoolCrash) {

TEST_CASE(StatelessCleanup) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    EXPECT_EQ(f.alive_count(), 2u);
    EXPECT_EQ(f.busy_count(), 1u);

    f.simulate_crash(0, false);

    EXPECT_EQ(f.alive_count(), 1u);
    EXPECT_EQ(f.busy_count(), 0u);
}

TEST_CASE(StatefulLostDocuments) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);
    f.add_stateful(true, 0);

    // Deterministic: first two go to worker 0 (least loaded), third to worker 1
    auto w0 = f.assign_worker(100);
    auto w1 = f.assign_worker(200);
    f.assign_worker(300);
    ASSERT_NE(w0, w1);

    // Record which docs worker 0 owns before crashing it
    llvm::SmallVector<std::uint32_t> expected;
    for(auto id: {100u, 200u, 300u}) {
        if(f.owner_of(id) == 0)
            expected.push_back(id);
    }

    f.simulate_crash(0, true);

    ASSERT_EQ(f.crash_reports.size(), 1u);
    auto& report = f.crash_reports[0];
    EXPECT_TRUE(report.stateful);
    EXPECT_EQ(report.worker_index, 0u);
    EXPECT_EQ(report.lost_documents.size(), expected.size());

    for(auto path_id: expected) {
        EXPECT_FALSE(f.has_owner(path_id));
        EXPECT_TRUE(llvm::is_contained(report.lost_documents, path_id));
    }

    // Other worker's documents are preserved
    for(auto id: {100u, 200u, 300u}) {
        if(!llvm::is_contained(expected, id))
            EXPECT_TRUE(f.has_owner(id));
    }
}

TEST_CASE(CrashReportsAnomaly) {
    /// Production trigger for the WorkerCrash anomaly: process_crash is the
    /// exact site the pool reports from.
    WorkerPoolFixture f;
    f.add_stateless(true, false);

    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    f.simulate_crash(0, false, 0, 11);

    ASSERT_EQ(trapped.size(), 1u);
    EXPECT_EQ(trapped[0], logging::AnomalyId::WorkerCrash);
}

TEST_CASE(SpawnFailReportsAnomaly) {
    /// Production trigger for the WorkerSpawnFail anomaly.
    WorkerPoolFixture f;

    std::vector<logging::AnomalyId> trapped;
    logging::set_anomaly_trap_for_testing([&](logging::AnomalyId id) { trapped.push_back(id); });

    WorkerPoolOptions opts;
    opts.self_path = "/nonexistent/clice-binary";
    opts.stateless_count = 1;
    opts.stateful_count = 0;
    EXPECT_FALSE(f.pool.start(opts));

    ASSERT_EQ(trapped.size(), 1u);
    EXPECT_EQ(trapped[0], logging::AnomalyId::WorkerSpawnFail);
}

TEST_CASE(OperationalErrorCodes) {
    /// Dispatch failures marked operational must never classify as anomalies.
    using worker::protocol::Error;
    EXPECT_TRUE(worker::is_operational_error(Error{worker::dispatch_errc::cancelled, "x"}));
    EXPECT_TRUE(
        worker::is_operational_error(Error{worker::dispatch_errc::worker_unavailable, "x"}));
    EXPECT_TRUE(worker::is_operational_error(Error{worker::dispatch_errc::worker_crashed, "x"}));
    EXPECT_TRUE(worker::is_operational_error(Error{worker::dispatch_errc::worker_restarting, "x"}));
    EXPECT_FALSE(worker::is_operational_error(Error{"plain failure"}));
}

TEST_CASE(TransportErrorClassification) {
    /// kota surfaces transport failures with the default RequestFailed code;
    /// remote handler errors carry specific codes and must pass through.
    using worker::protocol::Error;
    using worker::protocol::ErrorCode;
    EXPECT_TRUE(worker::is_transport_error(Error{"transport closed"}));
    EXPECT_FALSE(worker::is_transport_error(Error{ErrorCode::InternalError, "handler threw"}));
    EXPECT_FALSE(worker::is_transport_error(Error{ErrorCode::RequestCancelled, "cancelled"}));
}

TEST_CASE(CrashInfoSignal) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);

    f.simulate_crash(0, false, 0, 11);

    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_EQ(f.crash_reports[0].exit_signal, 11);
    EXPECT_EQ(f.crash_reports[0].exit_code, 0);
    EXPECT_FALSE(f.crash_reports[0].stateful);
}

TEST_CASE(CrashInfoExitCode) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);

    f.simulate_crash(0, false, 42, 0);

    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_EQ(f.crash_reports[0].exit_code, 42);
    EXPECT_EQ(f.crash_reports[0].exit_signal, 0);
}

TEST_CASE(WillRestartWithinBudget) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 1);

    auto should_restart = f.simulate_crash(0, false);

    EXPECT_TRUE(should_restart);
    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_TRUE(f.crash_reports[0].will_restart);
    EXPECT_EQ(f.crash_reports[0].crash_streak, 2u);
}

TEST_CASE(CrashBudgetExhausted) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 3);

    auto should_restart = f.simulate_crash(0, false);

    EXPECT_FALSE(should_restart);
    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_FALSE(f.crash_reports[0].will_restart);
    EXPECT_EQ(f.state(0), WorkerPoolFixture::SlotState::Dead);
}

TEST_CASE(HealthyUptimeResetsStreak) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 3);
    // Ran healthily past the reset threshold: this crash starts a new streak
    // instead of exhausting the budget.
    f.set_uptime(0, false, std::chrono::milliseconds(60'000));

    auto should_restart = f.simulate_crash(0, false);

    EXPECT_TRUE(should_restart);
    EXPECT_EQ(f.crash_streak(0), 1u);
}

TEST_CASE(ExpendableSkipsHosts) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.add_stateful(true);

    // A healthy document lives on worker 0 (least-loaded first pick)...
    ASSERT_EQ(f.assign_worker(1), 0u);

    // ...so a suspect compile may only sacrifice worker 1, and the
    // document's ownership moves there.
    ASSERT_EQ(f.assign_expendable(2), 1u);
    ASSERT_EQ(f.owner_of(2), 1u);

    // The current owner is kept while it hosts nothing else.
    ASSERT_EQ(f.assign_expendable(2), 1u);

    // With every live worker hosting someone else's document, there is
    // nothing to sacrifice: the probe stays armed instead of running.
    ASSERT_EQ(f.assign_worker(3), 0u);
    ASSERT_EQ(f.assign_expendable(4), SIZE_MAX);
}

TEST_CASE(SingleWorkerProbes) {
    WorkerPoolFixture f;
    f.add_stateful(true);

    // A single-worker pool has nothing to preserve by refusing the
    // probe: it runs on the lone worker rather than quarantining the
    // document until the session reopens.
    ASSERT_EQ(f.assign_worker(1), 0u);
    ASSERT_EQ(f.assign_expendable(2), 0u);
    ASSERT_EQ(f.owner_of(2), 0u);
}

TEST_CASE(SuspectCrashKeepsBudget) {
    WorkerPoolFixture f;
    f.add_stateful(true);
    f.set_crash_streak(0, true, 2);
    f.set_suspect_inflight(0, true, 1);

    // A crash with a suspect request (a quarantined document's probe) in
    // flight says something about the document, not the slot: the streak
    // stays where it was and the slot respawns.
    auto should_restart = f.simulate_crash(0, true);

    EXPECT_TRUE(should_restart);
    EXPECT_EQ(f.crash_streak(0, true), 2u);
}

TEST_CASE(BackoffDelaySchedule) {
    WorkerPoolFixture f;
    using ms = std::chrono::milliseconds;
    // First crash respawns immediately; later ones back off exponentially,
    // capped so a long streak cannot produce absurd delays.
    EXPECT_EQ(f.backoff_delay(0), ms(0));
    EXPECT_EQ(f.backoff_delay(1), ms(0));
    EXPECT_EQ(f.backoff_delay(2), ms(500));
    EXPECT_EQ(f.backoff_delay(3), ms(1000));
    EXPECT_EQ(f.backoff_delay(4), ms(2000));
    EXPECT_EQ(f.backoff_delay(10), ms(8000));
}

TEST_CASE(MarkDeadRemovesSlot) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    EXPECT_EQ(f.busy_count(), 1u);
    f.mark_dead(0);

    EXPECT_EQ(f.state(0), WorkerPoolFixture::SlotState::Dying);
    EXPECT_EQ(f.generation(0), 1u);
    EXPECT_EQ(f.busy_count(), 0u);
    // The dying slot still counts as future capacity (its verdict is
    // pending), but is not schedulable.
    EXPECT_EQ(f.capacity(), 2u);
    EXPECT_EQ(f.pick_idle(), 1u);

    // Idempotent: a second mark (e.g. the monitor's) is a no-op.
    f.mark_dead(0);
    EXPECT_EQ(f.generation(0), 1u);
}

TEST_CASE(ReleaseIdempotent) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    EXPECT_EQ(f.busy_count(), 1u);
    f.release_slot(0);
    EXPECT_EQ(f.busy_count(), 0u);
    f.release_slot(0);
    EXPECT_EQ(f.busy_count(), 0u);
}

TEST_CASE(CrashThenRelease) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    f.simulate_crash(0, false);
    EXPECT_EQ(f.busy_count(), 0u);

    // StatelessSlot destructor would call release_slot — must not underflow
    f.release_slot(0);
    EXPECT_EQ(f.busy_count(), 0u);
}

TEST_CASE(StatefulCrashClearsOwnership) {
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

    EXPECT_FALSE(f.has_owner(10));
    EXPECT_EQ(f.stateful_owned(idx0), 0u);
    EXPECT_EQ(f.stateful_owned(other_idx), other_owned_before);
}

TEST_CASE(AIMDBackoff) {
    WorkerPoolFixture f;
    f.set_low_limit(8);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 6u);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 4u);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 3u);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 2u);
}

TEST_CASE(AIMDMinimum) {
    WorkerPoolFixture f;
    f.set_low_limit(1);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 1u);
}

TEST_CASE(CrashAppliesBackoff) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_low_limit(8);

    f.simulate_crash(0, false);

    EXPECT_EQ(f.low_limit(), 6u);
}

TEST_CASE(IdleStatelessCrash) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    EXPECT_EQ(f.alive_count(), 2u);
    EXPECT_EQ(f.busy_count(), 0u);

    f.simulate_crash(0, false);

    EXPECT_EQ(f.alive_count(), 1u);
    EXPECT_EQ(f.busy_count(), 0u);
    EXPECT_FALSE(f.worker_alive(0));
}

TEST_CASE(RapidCrashSequence) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_low_limit(8);

    f.simulate_crash(0, false);
    f.simulate_crash(1, false);
    f.simulate_crash(2, false);

    EXPECT_EQ(f.alive_count(), 0u);
    EXPECT_EQ(f.busy_count(), 0u);
    EXPECT_EQ(f.crash_reports.size(), 3u);
    EXPECT_LT(f.low_limit(), 8u);
}

TEST_CASE(BackoffSetsCooldown) {
    WorkerPoolFixture f;
    f.set_low_limit(8);
    EXPECT_EQ(f.get_backoff_cooldown(), 0u);
    f.apply_backoff();
    EXPECT_GT(f.get_backoff_cooldown(), 0u);
}

TEST_CASE(CrashSetsCooldown) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_low_limit(8);

    f.simulate_crash(0, false);

    EXPECT_EQ(f.low_limit(), 6u);
    EXPECT_GT(f.get_backoff_cooldown(), 0u);
}

TEST_CASE(StatefulCrashNoCooldown) {
    WorkerPoolFixture f;
    f.add_stateful(true, 0);

    f.simulate_crash(0, true);

    EXPECT_EQ(f.get_backoff_cooldown(), 0u);
}

TEST_CASE(DeadPoolReturnsError) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_low_limit(1);
    f.set_max_crash_streak(0);

    f.simulate_crash(0, false);
    EXPECT_EQ(f.alive_count(), 0u);
    EXPECT_EQ(f.capacity(), 0u);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto idx = co_await f.acquire_slot(worker::Priority::High);
        EXPECT_EQ(idx, SIZE_MAX);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(DeadPoolDrainsPending) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    f.set_max_crash_streak(0);

    auto r = f.test_dead_pool_drain();
    EXPECT_TRUE(r.drained);
    EXPECT_EQ(r.assigned_worker, SIZE_MAX);
    EXPECT_EQ(f.high_queue_size(), 0u);
}

TEST_CASE(SlotGenGuard) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);
    EXPECT_TRUE(f.test_slot_gen_guard());
}

TEST_CASE(PendingClaimGenGuard) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.set_low_limit(1);
    EXPECT_TRUE(f.test_pending_gen_guard());
}

TEST_CASE(CrashBudgetBoundary) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_crash_streak(3);
    f.set_crash_streak(0, false, 2);

    // Exactly at the budget: the streak reaches 3 == max, still restarts.
    EXPECT_TRUE(f.simulate_crash(0, false));
    EXPECT_EQ(f.crash_streak(0), 3u);

    // One past the budget: give up.
    EXPECT_FALSE(f.simulate_crash(0, false));
    EXPECT_EQ(f.state(0), WorkerPoolFixture::SlotState::Dead);
}

TEST_CASE(StatefulDyingRejected) {
    // A request landing in the restart window never reached a worker: it
    // fails with worker_restarting, not worker_crashed, so crash accounting
    // (document quarantine) does not blame the document for the window.
    WorkerPoolFixture f;
    f.add_stateful();
    f.assign_worker(7);
    f.mark_dead(0, true);

    bool done = false;
    f.run([&]() -> kota::task<> {
        auto result = co_await f.pool.send_stateful(7, worker::DocumentLinkParams{"/x.cpp"});
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_restarting);
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(DyingSlotKeepsWaiters) {
    // A slot marked dead but without a verdict still counts as future
    // capacity: waiters must keep waiting for the respawn instead of
    // failing out.
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_low_limit(1);

    f.mark_dead(0);
    EXPECT_EQ(f.test_low_dispatch(1).dispatched, 0u);
    EXPECT_EQ(f.low_queue_size(), 0u);  // helper destroys its waiters
}

TEST_CASE(MaxLowLimitShrinks) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_low_limit(2);
    f.set_max_crash_streak(0);

    f.simulate_crash(0, false);

    // Capacity dropped to 2; the derived ceiling reserves one for high and
    // clamps the stored allowance of 2 down to it.
    EXPECT_EQ(f.max_low_limit(), 1u);
    EXPECT_EQ(f.effective_low_limit(), 1u);
}

};  // TEST_SUITE(WorkerPoolCrash)

TEST_SUITE(WorkerPoolMemory) {

TEST_CASE(PreemptKillsLowWorkers) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);   // busy low
    f.add_stateless(true, true, false);  // busy high — must survive
    f.set_low_limit(2);

    f.preempt(2);

    EXPECT_EQ(f.state(0), WorkerPoolFixture::SlotState::Dying);
    EXPECT_TRUE(f.preempted(0));
    EXPECT_EQ(f.state(1), WorkerPoolFixture::SlotState::Alive);
    EXPECT_TRUE(f.is_busy(1));
    EXPECT_EQ(f.low_busy(), 0u);
    // Preemption is not a crash: no report, no crash accounting.
    EXPECT_TRUE(f.crash_reports.empty());
    EXPECT_EQ(f.crash_streak(0), 0u);
}

TEST_CASE(PreemptCreditsHealthyRun) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.set_crash_streak(0, false, 2);
    f.set_uptime(0, false, std::chrono::milliseconds(60'000));

    f.preempt(1);

    // The healthy interval before the preemption clears the stale streak,
    // exactly as the next crash's accounting would have.
    EXPECT_EQ(f.crash_streak(0), 0u);
}

TEST_CASE(PreemptKeepsYoungStreak) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.set_crash_streak(0, false, 2);
    f.set_uptime(0, false, std::chrono::milliseconds(0));

    f.preempt(1);

    // A slot that has not yet earned the healthy credit keeps its streak:
    // preemption neither punishes nor forgives.
    EXPECT_EQ(f.crash_streak(0), 2u);
}

TEST_CASE(SevereMemoryTick) {
    WorkerPoolFixture f;
    f.add_stateless(true, true, true);
    f.add_stateless(true, true, true);
    f.add_stateless(true, false);
    f.set_low_limit(2);

    f.tick_memory(0.05);

    // Floor the allowance, remember the recovery target, kill the low work.
    EXPECT_EQ(f.low_limit(), 1u);
    EXPECT_EQ(f.w_max(), 2u);
    EXPECT_EQ(f.low_busy(), 0u);
    EXPECT_EQ(f.state(0), WorkerPoolFixture::SlotState::Dying);
    EXPECT_EQ(f.state(1), WorkerPoolFixture::SlotState::Dying);
}

TEST_CASE(PressureDecrementsLimit) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);

    f.tick_memory(0.15);
    EXPECT_EQ(f.low_limit(), 1u);
    EXPECT_EQ(f.w_max(), 2u);
}

TEST_CASE(RecoveryClosesGap) {
    WorkerPoolFixture f;
    for(int i = 0; i < 9; ++i)
        f.add_stateless();
    f.set_low_limit(2);
    // Simulate an earlier reduction from 8.
    f.set_w_max(8);

    f.tick_memory(0.5);
    EXPECT_EQ(f.low_limit(), 5u);  // gap 6 → +3
    f.tick_memory(0.5);
    EXPECT_EQ(f.low_limit(), 6u);  // gap 3 → +1 (integer halving)
}

TEST_CASE(CooldownSkipsDecrement) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.add_stateless();
    f.set_low_limit(2);
    f.apply_backoff();  // low_limit -> 1, cooldown = 3

    f.set_low_limit(2);
    f.tick_memory(0.15);
    // Cooldown consumed instead of a second decrement.
    EXPECT_EQ(f.low_limit(), 2u);
    EXPECT_LT(f.get_backoff_cooldown(), 3u);
}

};  // TEST_SUITE(WorkerPoolMemory)

TEST_SUITE(WorkerPoolIntegration) {

TEST_CASE(StartAndStop) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 1));
        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(StopIsPrompt) {
    // Regression guard: stop() must cancel the monitor loop's 3s poll
    // sleep instead of waiting it out.
    WorkerPoolFixture f;
    std::chrono::milliseconds elapsed{};
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        auto begin = std::chrono::steady_clock::now();
        co_await f.stop();
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin);
    });
    EXPECT_LT(elapsed.count(), 1500);
}

TEST_CASE(StatelessRequest) {
    TempDir tmp;
    tmp.touch("test.cpp", "int x = 1;\n");
    auto src = tmp.path("test.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        co_await kota::sleep(500);

        worker::BuildParams params;
        params.priority = worker::Priority::Low;
        params.kind = worker::BuildKind::Index;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        auto result = co_await f.pool.send_stateless(params);
        EXPECT_TRUE(result.has_value());
        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(CrashAndRestart) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));

        auto pid = f.worker_pid(0);
        EXPECT_GT(pid, 0);

        f.kill_worker(0);

        // Wait for respawn: PID must change (not just alive, which is
        // true before the kill is processed).
        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(f.worker_alive(0) && f.worker_pid(0) != pid)
                break;
        }
        EXPECT_TRUE(f.worker_alive(0));
        EXPECT_NE(f.worker_pid(0), pid);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(DeadSlotRevives) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(1, 0));
        f.set_revive_after(std::chrono::milliseconds(300));
        f.set_max_crash_streak(0);
        co_await kota::sleep(500);

        // The very first crash exceeds the zero budget: the slot dies...
        f.kill_worker(0);
        for(int i = 0; i < 50 && !f.slot_dead(0); ++i) {
            co_await kota::sleep(100);
        }
        EXPECT_TRUE(f.slot_dead(0));

        // ...and is revived with a fresh budget after the cooldown: the
        // pool never stays at zero workers forever.
        for(int i = 0; i < 100 && !f.worker_alive(0); ++i) {
            co_await kota::sleep(100);
        }
        EXPECT_TRUE(f.worker_alive(0));
        EXPECT_EQ(f.crash_streak(0), 0u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(InPlaceKeepsOwner) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(0, 2));
        co_await kota::sleep(500);

        // Two documents pin worker 0: it is the owner but not expendable.
        f.force_owner(7, 0);
        f.force_owner(8, 0);

        // An in-place suspect keeps owner routing — the AST lives there —
        // instead of migrating to the free worker like an isolated probe,
        // and its budget exemption unwinds once the reply lands.
        auto result = co_await f.pool.send_stateful(7,
                                                    worker::DocumentLinkParams{"/x.cpp"},
                                                    {},
                                                    Suspect::InPlace);
        EXPECT_EQ(f.owner_of(7), 0u);
        EXPECT_EQ(f.suspect_inflight(0, true), 0u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(FloorAboveStartupKept) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        // A floor configured above the startup count survives start():
        // scale-up may grow past it later, and idle scale-down must hold
        // the configured warm set instead of shrinking back to startup.
        CO_ASSERT_TRUE(f.start(2, 0, /*min_stateless=*/4));
        EXPECT_EQ(f.min_stateless(), 4u);
        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(RetiringHoldsScaleUp) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        f.set_max_stateless(2);
        f.set_retiring(0);

        // The retiring worker still holds its process until the monitor
        // reaps it: a replacement now would run three processes against a
        // ceiling of two, worsening the pressure that retired it.
        EXPECT_FALSE(f.scale_up());
        EXPECT_EQ(f.stateless_count(), 2u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(RevivesSlotsGate) {
    // Revival — and with it the indexer's requeue-on-unavailable — is only
    // promised by a started pool with a nonzero cooldown.
    WorkerPoolFixture f;
    EXPECT_FALSE(f.pool.revives_slots());

    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(1, 0));
        EXPECT_TRUE(f.pool.revives_slots());
        f.set_revive_after(std::chrono::milliseconds(0));
        EXPECT_FALSE(f.pool.revives_slots());
        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(ScaleUpRevivesDead) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        // Cooldown far in the future: only scale-up can bring the slot back.
        f.set_revive_after(std::chrono::minutes(10));
        f.set_max_crash_streak(0);
        co_await kota::sleep(500);

        f.kill_worker(0);
        for(int i = 0; i < 50 && !f.slot_dead(0); ++i) {
            co_await kota::sleep(100);
        }
        CO_ASSERT_TRUE(f.slot_dead(0));

        // Scale-up pressure revives the dead slot instead of appending a
        // third worker next to it.
        CO_ASSERT_TRUE(f.scale_up());
        EXPECT_EQ(f.stateless_count(), 2u);
        for(int i = 0; i < 50 && !f.worker_alive(0); ++i) {
            co_await kota::sleep(100);
        }
        EXPECT_TRUE(f.worker_alive(0));
        EXPECT_EQ(f.crash_streak(0), 0u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(CrashNotification) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));

        f.kill_worker(0);

        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(!f.crash_reports.empty())
                break;
        }

        CO_ASSERT_FALSE(f.crash_reports.empty());
        EXPECT_FALSE(f.crash_reports[0].stateful);
        EXPECT_EQ(f.crash_reports[0].exit_signal, 9);
        EXPECT_TRUE(f.crash_reports[0].will_restart);
        EXPECT_EQ(f.crash_reports[0].crash_streak, 1u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(CrashDuringRequest) {
    TempDir tmp;
    tmp.touch("test.cpp", "int x = 1;\n");
    auto src = tmp.path("test.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        co_await kota::sleep(500);

        // Kill both workers, then send without yielding: the claim lands on
        // a dead-but-not-yet-reaped slot, and the pool must surface
        // worker_crashed instead of retrying internally.
        f.kill_worker(0);
        f.kill_worker(1);

        worker::BuildParams params;
        params.priority = worker::Priority::Low;
        params.kind = worker::BuildKind::Index;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        auto result = co_await f.pool.send_stateless(params);
        CO_ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, worker::dispatch_errc::worker_crashed);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(PreemptCancelsRequest) {
    TempDir tmp;
    tmp.touch("slow.cpp", "#include <vector>\n#include <string>\nint x = 1;\n");
    auto src = tmp.path("slow.cpp");

    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(2, 0));
        co_await kota::sleep(500);

        worker::BuildParams params;
        params.priority = worker::Priority::Low;
        params.kind = worker::BuildKind::Index;
        params.file = src;
        params.directory = "/tmp";
        params.arguments = make_args(src);

        worker::protocol::integer code = 0;
        kota::task_group<> group(f.loop);
        auto sender = [&]() -> kota::task<> {
            auto result = co_await f.pool.send_stateless(params);
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
        EXPECT_EQ(code, worker::dispatch_errc::cancelled);

        // Preemption is not a crash: no report, and the worker comes back
        // with an untouched budget.
        EXPECT_TRUE(f.crash_reports.empty());
        for(int i = 0; i < 50; ++i) {
            co_await kota::sleep(100);
            if(f.worker_alive(0))
                break;
        }
        EXPECT_TRUE(f.worker_alive(0));
        EXPECT_EQ(f.crash_streak(0), 0u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

TEST_CASE(ScaleUpKeepsLimit) {
    WorkerPoolFixture f;
    bool done = false;
    f.run([&]() -> kota::task<> {
        CO_ASSERT_TRUE(f.start(3, 0));
        // Simulate pressure having reduced the allowance below the ceiling.
        f.set_low_limit(1);

        CO_ASSERT_TRUE(f.scale_up());

        // The new worker adds exactly one to the allowance; it must not
        // reset the reduction back to the ceiling (which is now 3).
        EXPECT_EQ(f.max_low_limit(), 3u);
        EXPECT_EQ(f.low_limit(), 2u);

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

};  // TEST_SUITE(WorkerPoolIntegration)

}  // namespace

}  // namespace clice::testing
