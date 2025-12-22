#pragma once

#include "Task.h"

#include "llvm/ADT/ArrayRef.h"

namespace clice::async {

namespace awaiter {

struct event {
    bool ready;
    llvm::SmallVectorImpl<promise_base*>& awaiters;

    bool await_ready() const noexcept {
        return ready;
    }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        awaiters.emplace_back(&handle.promise());
    }

    void await_resume() const noexcept {}
};

}  // namespace awaiter

class Event {
public:
    Event() = default;

    void set() {
        ready = true;
        for(auto* awaiter: awaiters) {
            awaiter->schedule();
        }
    }

    void clear() {
        ready = false;
        awaiters.clear();
    }

    auto operator co_await() {
        return awaiter::event{ready, awaiters};
    }

private:
    bool ready = false;
    llvm::SmallVector<promise_base*, 4> awaiters;
};

}  // namespace clice::async
