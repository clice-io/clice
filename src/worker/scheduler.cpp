module clice;

import :support.logging;
import :worker.scheduler;

namespace clice {

using worker::Priority;

static std::uint64_t less(std::uint64_t a, std::uint64_t b) {
    return a > b ? a - b : 0;
}

void WorkerScheduler::submit(RequestId id,
                             Priority priority,
                             RequestCost cost,
                             bool build,
                             SchedulerTime now) {
    requests.emplace(id,
                     Request{.id = id,
                             .priority = priority,
                             .build = build,
                             .peak = cost.peak,
                             .floor = cost.floor,
                             .submitted = now});
    if(priority == Priority::High) {
        high_queue.push_back(id);
        last_activity = now;
    } else {
        low_queue.push_back(id);
    }
    decide(now);
}

void WorkerScheduler::cancel(RequestId id, SchedulerTime now) {
    auto it = requests.find(id);
    // The request ended first: its worker died, or its answer is on the way.
    if(it == requests.end()) {
        return;
    }
    auto& request = it->second;
    if(!request.slot) {
        std::erase(request.priority == Priority::High ? high_queue : low_queue, id);
        requests.erase(it);
        decide(now);
        return;
    }
    if(!request.cancel_asked) {
        request.cancel_asked = now;
        actions.push_back(scheduler::Cancel{.request = id});
    }
}

void WorkerScheduler::raise(RequestId id, SchedulerTime now) {
    auto it = requests.find(id);
    if(it == requests.end() || it->second.priority == Priority::High) {
        return;
    }
    auto& request = it->second;
    request.priority = Priority::High;
    if(!request.slot) {
        std::erase(low_queue, id);
        high_queue.push_back(id);
    }
    last_activity = now;
    decide(now);
}

std::uint64_t WorkerScheduler::finish(RequestId id, SchedulerTime now) {
    auto it = requests.find(id);
    // Gone with its worker.
    if(it == requests.end()) {
        return 0;
    }
    auto request = std::move(it->second);
    requests.erase(it);
    if(request.slot) {
        slots.at(*request.slot).request.reset();
    } else {
        // Its sender gave up while it waited: the pool is stopping.
        std::erase(request.priority == Priority::High ? high_queue : low_queue, id);
    }
    if(request.priority == Priority::High) {
        last_activity = now;
    } else if(request.slot && !request.cancel_asked && request.max_memory > 0) {
        peaks.insert(ranges::upper_bound(peaks, request.max_memory), request.max_memory);
    }
    last_busy = now;
    decide(now);
    return request.max_memory;
}

void
    WorkerScheduler::stateful_begin(RequestId id, std::size_t slot, bool build, SchedulerTime now) {
    requests.emplace(id,
                     Request{.id = id,
                             .priority = Priority::High,
                             .stateful = true,
                             .build = build,
                             .submitted = now,
                             .slot = slot,
                             .order = next_order});
    next_order += 1;
    last_activity = now;
    decide(now);
}

void WorkerScheduler::stateful_started(RequestId id, SchedulerTime now) {
    // Gone with its worker.
    if(auto it = requests.find(id); it != requests.end()) {
        it->second.started = now;
    }
}

void WorkerScheduler::stateful_end(RequestId id, SchedulerTime now) {
    requests.erase(id);
    last_activity = now;
}

void WorkerScheduler::reported_peak(std::size_t slot, std::uint64_t memory) {
    auto& running = slots.at(slot).request;
    // The request ended first: its sender cancelled it while it waited
    // for the reply, or the pool is stopping.
    if(!running) {
        return;
    }
    auto& request = requests.at(*running);
    request.max_memory = std::max(request.max_memory, memory);
}

void WorkerScheduler::worker_ready(std::size_t slot, SchedulerTime now) {
    slots[slot] = Slot{.slot = slot};
    last_busy = now;
    decide(now);
}

void WorkerScheduler::worker_gone(std::size_t slot, bool stateful, SchedulerTime now) {
    std::erase_if(requests, [&](const auto& entry) {
        auto& request = entry.second;
        return request.stateful == stateful && request.slot == slot;
    });
    if(!stateful) {
        slots.erase(slot);
    }
    decide(now);
}

void WorkerScheduler::worker_exited(std::size_t slot) {
    if(memory_kill == slot) {
        memory_kill.reset();
    }
}

void WorkerScheduler::user_activity(SchedulerTime now) {
    last_activity = now;
    decide(now);
}

void WorkerScheduler::sample(SchedulerSample sample,
                             llvm::ArrayRef<WorkerSample> workers,
                             SchedulerTime now) {
    for(auto& worker: workers) {
        auto& slot = slots.at(worker.slot);
        slot.memory = worker.memory;
        if(!slot.request) {
            continue;
        }
        auto& request = requests.at(*slot.request);
        if(request.sampled && now > *request.sampled) {
            auto seconds = chrono::duration<double>(now - *request.sampled).count();
            request.growth =
                std::max(0.0, (double(worker.memory) - double(request.memory)) / seconds);
        }
        request.sampled = now;
        request.memory = worker.memory;
        request.max_memory = std::max(request.max_memory, worker.memory);
        if(!request.start_cpu || worker.cpu != request.last_cpu) {
            request.start_cpu = request.start_cpu.value_or(worker.cpu);
            request.last_cpu = worker.cpu;
            request.cpu_moved = now;
        }
    }
    last_sample = sample;
    if(busy()) {
        last_busy = now;
    }
    check_memory(now);
    check_deadlines(now);
    decide(now);
    scale(now);
}

std::size_t WorkerScheduler::background_limit(SchedulerTime now) const {
    return user_active(now) ? 1 : options.background_jobs;
}

chrono::milliseconds WorkerScheduler::sample_interval() const {
    return busy() ? chrono::seconds(1) : chrono::seconds(3);
}

std::size_t WorkerScheduler::running_on(std::size_t slot, bool stateful) const {
    return ranges::count_if(requests, [&](const auto& entry) {
        return entry.second.stateful == stateful && entry.second.slot == slot;
    });
}

bool WorkerScheduler::runs_alone(std::size_t slot) const {
    auto it = slots.find(slot);
    return it != slots.end() && it->second.request && requests_running() == 1;
}

std::size_t WorkerScheduler::requests_running() const {
    return ranges::count_if(requests, [](const auto& entry) {
        return entry.second.stateful || entry.second.slot.has_value();
    });
}

std::uint64_t WorkerScheduler::reached(std::size_t slot) const {
    auto it = slots.find(slot);
    if(it == slots.end() || !it->second.request) {
        return 0;
    }
    return requests.at(*it->second.request).max_memory;
}

std::uint64_t WorkerScheduler::unknown_peak() const {
    if(peaks.empty()) {
        return default_peak;
    }
    auto p75 = peaks[peaks.size() * 3 / 4];
    return peaks.size() >= trusted_peaks ? p75 : std::max(default_peak, p75);
}

bool WorkerScheduler::user_active(SchedulerTime now) const {
    if(last_activity && now - *last_activity < foreground_hold) {
        return true;
    }
    return ranges::any_of(requests, [&](const auto& entry) {
        auto& request = entry.second;
        return request.priority == Priority::High &&
               now - request.submitted < foreground_request_hold;
    });
}

std::size_t WorkerScheduler::running(Priority priority) const {
    return ranges::count_if(slots, [&](const auto& entry) {
        auto& slot = entry.second;
        return slot.request && requests.at(*slot.request).priority == priority;
    });
}

std::uint64_t WorkerScheduler::estimate(const Request& request) const {
    auto peak = request.peak;
    if(peak == 0) {
        peak = unknown_peak();
        if(peak < default_peak && unknown_ahead(request) >= learned_runs) {
            peak = default_peak;
        }
    }
    peak = std::max(peak, request.floor);
    // One that outgrew its estimate has more to come: a quarter more is
    // assumed, as nothing tells how much.
    return request.memory < peak ? peak : request.memory + request.memory / 4;
}

std::size_t WorkerScheduler::unknown_ahead(const Request& request) const {
    return ranges::count_if(slots, [&](const auto& entry) {
        auto& slot = entry.second;
        if(!slot.request || *slot.request == request.id) {
            return false;
        }
        auto& other = requests.at(*slot.request);
        return other.priority == Priority::Low && other.peak == 0 &&
               (!request.slot || other.order < request.order);
    });
}

std::uint64_t WorkerScheduler::growth_due() const {
    std::uint64_t due = 0;
    for(auto& [_, slot]: slots) {
        if(!slot.request) {
            continue;
        }
        auto& request = requests.at(*slot.request);
        if(request.priority == Priority::Low) {
            due += less(estimate(request), request.memory);
        }
    }
    return due;
}

std::uint64_t WorkerScheduler::growth_soon() const {
    auto horizon = chrono::duration<double>(memory_horizon).count();
    std::uint64_t soon = 0;
    for(auto& [_, slot]: slots) {
        if(!slot.request) {
            continue;
        }
        auto& request = requests.at(*slot.request);
        if(request.priority != Priority::Low || request.killed) {
            continue;
        }
        soon += std::min(std::uint64_t(request.growth * horizon),
                         less(estimate(request), request.memory));
    }
    return soon;
}

bool WorkerScheduler::memory_admits(const Request& request, const Slot& slot) const {
    auto headroom = last_sample.headroom;
    auto reserve = last_sample.reserve;
    // With nothing beside it, waiting would free no memory. The only request
    // starts where it can grow by a share of the reserve before check_memory
    // takes it for too big for the machine; beside a stateful one, where it
    // would not be killed at once.
    if(running(Priority::Low) == 0) {
        return headroom >= (requests_running() == 0 ? 2 * reserve / lone_share : reserve);
    }
    // The worker's own memory is the request's to reuse. Sampled before its
    // last request ended, it still counts what that request freed since,
    // and the headroom sampled with it lacks the same bytes.
    auto room = headroom + slot.memory;
    return room >= reserve + growth_due() + estimate(request);
}

WorkerScheduler::Slot* WorkerScheduler::idle_slot() {
    for(auto& [_, slot]: slots) {
        if(!slot.scaling_down && !slot.request) {
            return &slot;
        }
    }
    return nullptr;
}

bool WorkerScheduler::busy() const {
    return ranges::any_of(requests, [](const auto& entry) { return !entry.second.stateful; });
}

void WorkerScheduler::start(Request& request, Slot& slot, SchedulerTime now) {
    request.slot = slot.slot;
    request.order = next_order;
    next_order += 1;
    request.memory = slot.memory;
    slot.request = request.id;
    slot.ran = true;
    last_busy = now;
    actions.push_back(scheduler::Dispatch{.request = request.id, .slot = slot.slot});
}

void WorkerScheduler::cancel_newest_low(std::size_t count, SchedulerTime now) {
    llvm::SmallVector<Request*> low;
    for(auto& [_, slot]: slots) {
        if(slot.request) {
            auto& request = requests.at(*slot.request);
            if(request.priority == Priority::Low && !request.killed) {
                low.push_back(&request);
            }
        }
    }
    auto asked = ranges::count_if(low, [](Request* r) { return r->cancel_asked.has_value(); });
    ranges::sort(low, std::greater{}, &Request::order);
    for(auto* request: low) {
        if(static_cast<std::size_t>(asked) >= count) {
            break;
        }
        if(!request->cancel_asked) {
            request->cancel_asked = now;
            actions.push_back(scheduler::Cancel{.request = request->id});
            asked += 1;
        }
    }
}

void WorkerScheduler::kill(llvm::ArrayRef<const Request*> killed, scheduler::KillReason reason) {
    auto& first = *killed.front();
    auto slot = *first.slot;
    scheduler::Kill action{.slot = slot, .stateful = first.stateful, .reason = reason};
    for(auto* request: killed) {
        action.requests.push_back(request->id);
    }
    actions.push_back(std::move(action));
    for(auto& [_, other]: requests) {
        if(other.stateful == first.stateful && other.slot == slot) {
            other.killed = true;
        }
    }
}

void WorkerScheduler::decide(SchedulerTime now) {
    // Background work beyond what the user leaves it, and one background
    // request for each foreground request no idle worker takes, are asked
    // to stop.
    auto limit = background_limit(now);
    auto low = running(Priority::Low);
    std::size_t owed = low > limit ? low - limit : 0;
    if(!high_queue.empty() && !idle_slot()) {
        owed = std::max(owed, high_queue.size());
    }
    if(owed > 0) {
        cancel_newest_low(owed, now);
    }
    dispatch(now);
}

void WorkerScheduler::dispatch(SchedulerTime now) {
    while(!high_queue.empty()) {
        auto* slot = idle_slot();
        if(!slot) {
            break;
        }
        start(requests.at(high_queue.front()), *slot, now);
        high_queue.pop_front();
    }
    auto limit = background_limit(now);
    while(!low_queue.empty() && running(Priority::Low) < limit) {
        auto* slot = idle_slot();
        if(!slot) {
            break;
        }
        auto& head = requests.at(low_queue.front());
        if(memory_admits(head, *slot)) {
            memory_wait.reset();
            start(head, *slot, now);
            low_queue.pop_front();
            continue;
        }
        if(!memory_wait || memory_wait->first != head.id) {
            memory_wait = {head.id, now};
        }
        if(!memory_logged || now - *memory_logged >= memory_log_interval) {
            memory_logged = now;
            LOG_INFO(
                "Background work waits for memory: {} MiB headroom, {} MiB of the idle "
                "worker, {} MiB reserve, {} MiB growth due, {} MiB expected, {} running",
                last_sample.headroom >> 20,
                slot->memory >> 20,
                last_sample.reserve >> 20,
                growth_due() >> 20,
                estimate(head) >> 20,
                running(Priority::Low));
        }
        // While the head waits for memory, a later request that fits takes
        // the room; past long_wait the queue drains for the head instead,
        // or it would never get its room.
        if(now - memory_wait->second >= long_wait) {
            break;
        }
        auto behind = low_queue | views::drop(1) | views::take(backfill_depth);
        auto later = ranges::find_if(behind, [&](RequestId id) {
            return memory_admits(requests.at(id), *slot);
        });
        if(later == behind.end()) {
            break;
        }
        auto id = *later;
        std::erase(low_queue, id);
        start(requests.at(id), *slot, now);
    }
}

WorkerScheduler::Slot* WorkerScheduler::heaviest_idle() {
    Slot* heaviest = nullptr;
    for(auto& [_, slot]: slots | views::reverse) {
        if(!slot.scaling_down && !slot.request && (!heaviest || slot.memory > heaviest->memory)) {
            heaviest = &slot;
        }
    }
    return heaviest;
}

bool WorkerScheduler::scale_down_idle() {
    auto serving =
        ranges::count_if(slots, [](const auto& entry) { return !entry.second.scaling_down; });
    bool scaled_down = false;
    while(static_cast<std::size_t>(serving) > options.min_workers) {
        auto* slot = heaviest_idle();
        if(!slot) {
            break;
        }
        slot->scaling_down = true;
        serving -= 1;
        scaled_down = true;
        actions.push_back(scheduler::ScaleDown{.slot = slot->slot});
    }
    return scaled_down;
}

void WorkerScheduler::check_memory(SchedulerTime now) {
    if(memory_kill) {
        return;
    }
    // Several background requests are stopped before they run short, at the
    // rate they grow; a lone one only once it has, and the only stateless
    // request only as it is about to run through the reserve: killing it
    // takes it for too big for the machine.
    auto low = running(Priority::Low);
    auto reserve = last_sample.reserve;
    auto short_by = low > 1                               ? reserve + growth_soon()
                    : low == 1 && requests_running() == 1 ? reserve / lone_share + growth_soon()
                                                          : reserve;
    if(last_sample.headroom >= short_by && !(low > 1 && last_sample.swapping)) {
        return;
    }
    // The idle processes' memory is the cheapest to free, and a lone
    // request gets it before it is found too big.
    if(scale_down_idle() && low == 1) {
        return;
    }
    const Request* newest = nullptr;
    for(auto& [_, slot]: slots) {
        if(!slot.request) {
            continue;
        }
        auto& request = requests.at(*slot.request);
        if(request.priority == Priority::Low && !request.killed &&
           (!newest || request.order > newest->order)) {
            newest = &request;
        }
    }
    if(newest) {
        memory_kill = *newest->slot;
        // What it reached counts among the peaks: the units killed are the
        // big ones, and the finished ones alone would leave them out.
        peaks.insert(ranges::upper_bound(peaks, newest->memory), newest->memory);
        kill({newest}, scheduler::KillReason::Memory);
    }
}

void WorkerScheduler::check_deadlines(SchedulerTime now) {
    for(auto& [_, request]: requests) {
        if(request.stateful || !request.slot || request.killed) {
            continue;
        }
        if(request.cancel_asked && now - *request.cancel_asked > cancel_grace) {
            kill({&request}, scheduler::KillReason::CancelGrace);
            continue;
        }
        if(!request.start_cpu) {
            continue;
        }
        auto limit = request.build ? options.build_deadline : options.query_deadline;
        if(request.last_cpu - *request.start_cpu > limit) {
            kill({&request}, scheduler::KillReason::Deadline);
        } else if(now - request.cpu_moved > options.stall_deadline) {
            kill({&request}, scheduler::KillReason::Stalled);
        }
    }

    // Stateful: by worker, every overdue request among those its worker
    // began.
    std::map<std::size_t, llvm::SmallVector<const Request*>> late;
    for(auto& [_, request]: requests) {
        if(!request.stateful || request.killed || !request.started) {
            continue;
        }
        auto limit = request.build ? options.build_deadline : options.query_deadline;
        if(now - *request.started > limit) {
            late[*request.slot].push_back(&request);
        }
    }
    for(auto& [_, overdue]: late) {
        kill(overdue, scheduler::KillReason::Deadline);
    }
}

void WorkerScheduler::scale(SchedulerTime now) {
    auto processes = last_sample.stateless_processes;
    auto idle = idle_slot();
    if(!high_queue.empty() && !idle) {
        starved_since = starved_since.value_or(now);
        if(now - *starved_since >= starved_spawn && processes < options.max_workers) {
            starved_since.reset();
            actions.push_back(scheduler::Spawn{});
        }
        return;
    }
    starved_since.reset();

    // Background work waiting behind a pool idle time or memory shrank.
    if(!low_queue.empty() && !idle && running(Priority::Low) < background_limit(now) &&
       processes < std::min(options.background_jobs, options.max_workers) &&
       memory_admits(requests.at(low_queue.front()), Slot{})) {
        actions.push_back(scheduler::Spawn{});
        return;
    }

    if(busy() || now - last_busy < idle_scale_down) {
        return;
    }
    auto serving =
        ranges::count_if(slots, [](const auto& entry) { return !entry.second.scaling_down; });
    if(static_cast<std::size_t>(serving) > options.min_workers) {
        auto* heaviest = heaviest_idle();
        heaviest->scaling_down = true;
        last_busy = now;
        actions.push_back(scheduler::ScaleDown{.slot = heaviest->slot});
        return;
    }
    // At the floor, a fresh process joins one past replace_above; the next
    // scale-down takes the heavy one, which serves until then.
    bool heavy = ranges::any_of(slots, [](const auto& entry) {
        auto& slot = entry.second;
        return !slot.scaling_down && slot.ran && slot.memory > replace_above;
    });
    if(heavy && processes < options.max_workers) {
        last_busy = now;
        actions.push_back(scheduler::Spawn{});
    }
}

}  // namespace clice
