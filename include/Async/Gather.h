#pragma once

#include <thread>
#include <tuple>

#include "Event.h"
#include "Task.h"

namespace clice::async {

struct none {};

template <typename Task, typename V = typename std::remove_cvref_t<Task>::value_type>
using task_value_t = std::conditional_t<std::is_void_v<V>, none, V>;

template <typename... Tasks>
auto gather(Tasks&&... tasks) -> Task<std::tuple<task_value_t<Tasks>...>> {
    constexpr static std::size_t count = sizeof...(Tasks);

    Event event;
    std::size_t finished = 0;

    auto run_task = [&](auto& task) -> Task<task_value_t<decltype(task)>> {
        using V = typename std::remove_cvref_t<decltype(task)>::value_type;
        if constexpr(std::is_void_v<V>) {
            co_await task;
            /// Check if all tasks are finished. If so, set the event to
            /// resume the gather handle.
            finished += 1;
            if(finished == count) {
                event.set();
            }
            co_return none{};
        } else {
            auto result = co_await task;
            finished += 1;
            if(finished == count) {
                event.set();
            }
            co_return std::move(result);
        }
    };

    auto schedule_task = [&](auto& task) {
        auto core = run_task(task);
        core.schedule();
        return core;
    };

    std::tuple all = {schedule_task(tasks)...};

    /// Wait for all tasks to finish.
    co_await event;

    /// Return the results of all tasks.
    co_return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::make_tuple(std::get<Is>(all).result()...);
    }(std::make_index_sequence<count>{});
}

/// Run the tasks in parallel and return the results.
template <typename... Tasks>
auto run(Tasks&&... tasks) {
    auto core = gather(std::forward<Tasks>(tasks)...);
    core.schedule();
    async::run();
    assert(core.done() && "run: not done");
    return core.result();
}

template <ranges::input_range Range, typename Coroutine>
    requires requires(Coroutine coroutine, ranges::range_value_t<Range> value) {
        { coroutine(value) } -> std::same_as<Task<bool>>;
    }
Task<bool> gather(Range&& range,
                  Coroutine&& coroutine,
                  std::size_t concurrency = std::thread::hardware_concurrency()) {
    std::vector<Task<>> tasks;
    tasks.reserve(concurrency);

    auto iter = ranges::begin(range);
    auto end = ranges::end(range);

    Event event;
    std::size_t finished = 0;
    bool cancelled = false;

    auto run_task = [&](auto& value) -> async::Task<> {
        /// Execute the first task.
        auto task = coroutine(value);

        /// If any task fails, cancel all tasks and return false.
        if(auto result = co_await task; !result) {
            for(auto& task: tasks) {
                task.cancel();
                task.dispose();
            }
            cancelled = true;
            event.set();
            co_return;
        }

        finished += 1;

        /// Check if still have tasks to run. If so, run the next task.
        while(iter != end) {
            auto task = coroutine(*iter);
            iter++;
            finished -= 1;

            if(auto result = co_await task; !result) {
                for(auto& task: tasks) {
                    task.cancel();
                    task.dispose();
                }
                cancelled = true;
                event.set();
                co_return;
            }

            finished += 1;
        }

        /// Check if all tasks are finished. If so, set the event to
        /// resume the gather handle.
        if(finished == tasks.size()) {
            event.set();
        }
    };

    /// Fill tasks.
    while(iter != end && tasks.size() < concurrency) {
        tasks.emplace_back(run_task(*iter));
        tasks.back().schedule();
        iter++;
    }

    co_await event;

    co_return !cancelled;
}

}  // namespace clice::async
