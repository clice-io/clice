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
        w.alive = alive;
        w.busy = busy;
        w.low_priority = busy && low;
        if(alive)
            pool.alive_stateless_count += 1;
        if(busy) {
            pool.stateless_busy_count += 1;
            if(low)
                pool.low_busy_count += 1;
        }
    }

    void add_stateful(bool alive = true, std::size_t owned = 0) {
        auto idx = pool.stateful_workers.size();
        pool.stateful_workers.push_back(WorkerPool::WorkerProcess{});
        auto& w = pool.stateful_workers.back();
        w.name = "SF-" + std::to_string(idx);
        w.alive = alive;
        w.owned_documents = owned;
    }

    void set_limits(std::size_t low, std::size_t max_low) {
        pool.low_limit = low;
        pool.max_low_limit = max_low;
    }

    void set_max_restarts(unsigned n) {
        pool.options.max_restarts = n;
    }

    void set_restart_count(std::size_t idx, bool stateful, unsigned count) {
        auto& workers = stateful ? pool.stateful_workers : pool.stateless_workers;
        workers[idx].restart_count = count;
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

    bool start(std::uint32_t stateless = 2, std::uint32_t stateful = 0) {
        WorkerPoolOptions opts;
        opts.self_path = clice_binary();
        opts.stateless_count = stateless;
        opts.stateful_count = stateful;
        return pool.start(opts);
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

    std::size_t alive_count() const {
        return pool.alive_stateless_count;
    }

    std::size_t busy_count() const {
        return pool.stateless_busy_count;
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

    bool worker_alive(std::size_t idx) const {
        return pool.stateless_workers[idx].alive;
    }

    int worker_pid(std::size_t idx) const {
        return pool.stateless_workers[idx].proc.pid();
    }

    struct PriorityResult {
        bool high_dispatched;
        bool low_dispatched;
    };

    unsigned get_backoff_cooldown() const {
        return pool.backoff_cooldown;
    }

    std::size_t max_low_limit() const {
        return pool.max_low_limit;
    }

    std::size_t high_queue_size() const {
        return pool.high_queue.size();
    }

    std::size_t low_queue_size() const {
        return pool.low_queue.size();
    }

    PriorityResult test_priority_dispatch(std::size_t release_idx) {
        auto high = std::make_unique<WorkerPool::PendingStateless>(worker::Priority::High);
        auto low = std::make_unique<WorkerPool::PendingStateless>(worker::Priority::Low);
        pool.high_queue.push_back(high.get());
        high->queue = &pool.high_queue;
        pool.low_queue.push_back(low.get());
        low->queue = &pool.low_queue;
        pool.release_stateless_slot(release_idx);
        return {high->ready.is_set(), low->ready.is_set()};
    }

    std::size_t test_low_dispatch(std::size_t count) {
        llvm::SmallVector<std::unique_ptr<WorkerPool::PendingStateless>> pending;
        for(std::size_t i = 0; i < count; ++i) {
            pending.push_back(
                std::make_unique<WorkerPool::PendingStateless>(worker::Priority::Low));
            pool.low_queue.push_back(pending.back().get());
            pending.back()->queue = &pool.low_queue;
        }
        pool.try_dispatch_pending();
        std::size_t dispatched = 0;
        for(auto& p: pending)
            if(p->ready.is_set())
                dispatched += 1;
        return dispatched;
    }

    bool test_slot_raii(std::size_t idx) {
        pool.stateless_workers[idx].busy = true;
        pool.stateless_busy_count += 1;
        { WorkerPool::StatelessSlot slot(pool, idx); }
        return !pool.stateless_workers[idx].busy && pool.stateless_busy_count == 0 &&
               pool.low_busy_count == 0;
    }

    struct ReleaseResult {
        bool pending_before;
        bool dispatched_after;
    };

    ReleaseResult test_release_dispatches() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        bool before = pending->ready.is_set();
        pool.release_stateless_slot(0);
        bool after = pending->ready.is_set();
        return {before, after};
    }

    bool test_pending_cleanup_high() {
        {
            WorkerPool::PendingStateless pending(worker::Priority::High);
            pool.high_queue.push_back(&pending);
            pending.queue = &pool.high_queue;
            if(pool.high_queue.size() != 1)
                return false;
        }
        return pool.high_queue.empty();
    }

    bool test_pending_cleanup_low() {
        {
            WorkerPool::PendingStateless pending(worker::Priority::Low);
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
        auto pending = std::make_unique<WorkerPool::PendingStateless>(worker::Priority::High);
        pool.high_queue.push_back(pending.get());
        pending->queue = &pool.high_queue;
        simulate_crash(0, false);
        return {pending->ready.is_set(), pending->assigned_worker};
    }

    bool test_slot_gen_guard() {
        // Old StatelessSlot should NOT release a slot that was crash-replaced.
        {
            WorkerPool::StatelessSlot slot(pool, 0);
            // Simulate respawn bumping restart_count.
            pool.stateless_workers[0].restart_count = 1;
            pool.stateless_workers[0].busy = true;
        }
        // Destructor saw mismatched gen → skipped release.
        return pool.stateless_workers[0].busy && pool.stateless_busy_count == 1;
    }

    DispatchResult test_dispatch_clears_queue() {
        auto pending = std::make_unique<WorkerPool::PendingStateless>(worker::Priority::High);
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

TEST_CASE(AcquireHighImmediate) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_limits(2, 2);
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
    f.set_limits(1, 1);
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
    f.set_limits(0, 0);
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
    f.set_limits(1, 1);
    auto r = f.test_priority_dispatch(0);
    EXPECT_TRUE(r.high_dispatched);
    EXPECT_FALSE(r.low_dispatched);
}

TEST_CASE(LowLimitEnforced) {
    WorkerPoolFixture f;
    f.add_stateless();
    f.add_stateless();
    f.set_limits(1, 1);
    EXPECT_EQ(f.test_low_dispatch(2), 1u);
    EXPECT_EQ(f.busy_count(), 1u);
}

TEST_CASE(ReleaseDispatchesPending) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_limits(1, 1);
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
    f.set_limits(1, 1);

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
    f.set_limits(1, 1);

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
    f.set_limits(1, 1);
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
    f.set_limits(1, 1);
    auto r = f.test_dispatch_clears_queue();
    EXPECT_TRUE(r.dispatched);
    EXPECT_TRUE(r.queue_cleared);
    EXPECT_TRUE(r.queue_ptr_nulled);
}

};  // TEST_SUITE(WorkerPoolScheduling)

TEST_SUITE(WorkerPoolCrash) {

TEST_CASE(StatelessCleanup) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.add_stateless(true, false);
    f.set_limits(2, 2);

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
    EXPECT_FALSE(worker::is_operational_error(Error{"plain failure"}));
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

TEST_CASE(WillRestart) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_restarts(3);
    f.set_restart_count(0, false, 1);

    auto should_restart = f.simulate_crash(0, false);

    EXPECT_TRUE(should_restart);
    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_TRUE(f.crash_reports[0].will_restart);
    EXPECT_EQ(f.crash_reports[0].restart_count, 1u);
}

TEST_CASE(MaxRestartsExceeded) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_max_restarts(3);
    f.set_restart_count(0, false, 3);

    auto should_restart = f.simulate_crash(0, false);

    EXPECT_FALSE(should_restart);
    ASSERT_EQ(f.crash_reports.size(), 1u);
    EXPECT_FALSE(f.crash_reports[0].will_restart);
}

TEST_CASE(ReleaseIdempotent) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_limits(1, 1);

    EXPECT_EQ(f.busy_count(), 1u);
    f.release_slot(0);
    EXPECT_EQ(f.busy_count(), 0u);
    f.release_slot(0);
    EXPECT_EQ(f.busy_count(), 0u);
}

TEST_CASE(CrashThenRelease) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_limits(1, 1);

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
    f.set_limits(8, 8);
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
    f.set_limits(1, 4);
    f.apply_backoff();
    EXPECT_EQ(f.low_limit(), 1u);
}

TEST_CASE(CrashAppliesBackoff) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_limits(8, 8);

    f.simulate_crash(0, false);

    EXPECT_EQ(f.low_limit(), 6u);
}

TEST_CASE(IdleStatelessCrash) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_limits(2, 2);

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
    f.set_limits(8, 8);

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
    f.set_limits(8, 8);
    EXPECT_EQ(f.get_backoff_cooldown(), 0u);
    f.apply_backoff();
    EXPECT_GT(f.get_backoff_cooldown(), 0u);
}

TEST_CASE(CrashSetsCooldown) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.set_limits(8, 8);

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
    f.set_limits(1, 1);
    f.set_max_restarts(0);

    f.simulate_crash(0, false);
    EXPECT_EQ(f.alive_count(), 0u);

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
    f.set_limits(1, 1);
    f.set_max_restarts(0);

    auto r = f.test_dead_pool_drain();
    EXPECT_TRUE(r.drained);
    EXPECT_EQ(r.assigned_worker, SIZE_MAX);
    EXPECT_EQ(f.high_queue_size(), 0u);
}

TEST_CASE(SlotGenGuard) {
    WorkerPoolFixture f;
    f.add_stateless(true, true);
    f.set_limits(1, 1);
    EXPECT_TRUE(f.test_slot_gen_guard());
}

TEST_CASE(MaxLowLimitClamped) {
    WorkerPoolFixture f;
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.add_stateless(true, false);
    f.set_limits(2, 2);
    f.set_max_restarts(0);

    f.simulate_crash(0, false);

    // max_low_limit should shrink to alive-1 (or alive if 1).
    EXPECT_EQ(f.max_low_limit(), 1u);
    EXPECT_LE(f.low_limit(), f.max_low_limit());
}

};  // TEST_SUITE(WorkerPoolCrash)

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
    // Regression guard: stop() must cancel monitor_memory()'s 3s poll
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

        co_await f.stop();
        done = true;
    });
    EXPECT_TRUE(done);
}

};  // TEST_SUITE(WorkerPoolIntegration)

}  // namespace

}  // namespace clice::testing
