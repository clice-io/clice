#pragma once

#include <cassert>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <source_location>

#include "Support/Format.h"

namespace clice::async {

template <typename T>
class Task;

struct promise_base {
    enum Flags : uint8_t {
        Empty = 0,

        /// The task is cancelled.
        Cancelled = 1,

        /// The coroutine handle will be destroyed when the task is done or cancelled.
        Disposable = 1 << 1,

        /// The coroutine is done or is cancelled and resumed, means it will never
        /// scheduled again.
        Finished = 1 << 2,
    };

    uint8_t flags;

    void* data;

    /// The coroutine handle that is waiting for the task to complete.
    /// If this is a top-level coroutine, it is empty.
    promise_base* continuation = nullptr;

    promise_base* next = nullptr;

    std::source_location location;

    template <typename Promise>
    void set(std::coroutine_handle<Promise> handle) {
        flags = Empty;
        data = handle.address();
    }

    auto handle() const noexcept {
        return std::coroutine_handle<>::from_address(data);
    }

    void schedule();

    bool done() const noexcept {
        return handle().done();
    }

    void destroy() {
        handle().destroy();
    }

    void cancel() {
        auto p = this;
        while(p) {
            p->flags |= Flags::Cancelled;
            p = p->next;
        }
    }

    bool cancelled() const noexcept {
        return flags & Flags::Cancelled;
    }

    void dispose() {
        flags |= Flags::Disposable;
    }

    bool disposable() const noexcept {
        return flags & Flags::Disposable;
    }

    void finish() {
        flags |= Flags::Finished;
    }

    bool finished() {
        return flags & Flags::Finished;
    }

    std::coroutine_handle<> resume_handle() {
        if(cancelled()) {
            /// If the task is cancelled and disposable, destroy the coroutine handle.
            auto p = this;
            while(p && p->cancelled()) {
                auto con = p->continuation;

                if(p->disposable()) {
                    p->destroy();
                } else {
                    p->finish();
                }

                p = con;
            }

            return std::noop_coroutine();
        } else {
            /// Otherwise, resume the coroutine handle.
            return handle();
        }
    }

    void resume() {
        resume_handle().resume();
    }
};

namespace awaiter {

/// The awaiter for the final suspend point of `Task`.
struct final {
    promise_base* continuation;

    bool await_ready() noexcept {
        return false;
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> current) noexcept {
        std::coroutine_handle<> handle = std::noop_coroutine();

        /// In the final suspend point, this coroutine is already done.
        /// So try to resume the waiting coroutine if it exists.
        if(continuation) {
            continuation->next = nullptr;
            handle = continuation->resume_handle();
        }

        /// Mark current coroutine as finished.
        current.promise().finish();

        if(current.promise().disposable()) {
            /// If this task is disposable, destroy the coroutine handle.
            current.destroy();
        }

        return handle;
    }

    void await_resume() noexcept {}
};

/// The awaiter for the `Task` type.
template <typename T, typename P>
struct task {
    std::coroutine_handle<P> handle;

    bool await_ready() noexcept {
        return false;
    }

    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> waiting) noexcept {
        /// Store the waiting coroutine in the promise for later scheduling.
        /// It will be scheduled in the final suspend point.
        assert(!handle.promise().continuation && "await_suspend: already waiting");
        handle.promise().continuation = &waiting.promise();
        waiting.promise().next = &handle.promise();

        /// If this `Task` is awaited from another coroutine, we should schedule
        /// the this task first.
        return handle.promise().resume_handle();
    }

    T await_resume() noexcept {
        if constexpr(!std::is_void_v<T>) {
            assert(handle.promise().value.has_value() && "await_resume: value not set");
            return std::move(*handle.promise().value);
        }
    }
};

}  // namespace awaiter

template <typename T = void>
class Task {
public:
    template <typename V>
    struct promise_result {
        std::optional<V> value;

        template <typename U>
        void return_value(U&& val) noexcept {
            assert(!value.has_value() && "return_value: value already set");
            value.emplace(std::forward<U>(val));
        }
    };

    // WORKAROUND: GCC bug - full specialization in non-namespace scope not supported
    // see: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85282
    template <std::same_as<void> V>
    struct promise_result<V> {
        void return_void() noexcept {}
    };

    struct promise_type : promise_base, promise_result<T> {
        promise_type(std::source_location location = std::source_location::current()) {
            set(handle());
            this->location = location;
        };

        auto get_return_object() {
            return Task<T>(handle());
        }

        auto initial_suspend() {
            return std::suspend_always();
        }

        auto final_suspend() noexcept {
            return awaiter::final{continuation};
        }

        void unhandled_exception() {
            std::abort();
        }

        auto handle() {
            return std::coroutine_handle<promise_type>::from_promise(*this);
        }
    };

    using coroutine_handle = std::coroutine_handle<promise_type>;

    using value_type = T;

public:
    Task() = default;

    Task(coroutine_handle handle) : core(handle) {}

    Task(const Task&) = delete;

    Task(Task&& other) noexcept : core(other.core) {
        other.core = nullptr;
    }

    Task& operator=(const Task&) = delete;

    Task& operator=(Task&& other) noexcept {
        if(core) {
            core.destroy();
        }
        core = other.core;
        other.core = nullptr;
        return *this;
    }

    ~Task() {
        if(core) {
            core.destroy();
        }
    }

public:
    coroutine_handle handle() const noexcept {
        return core;
    }

    coroutine_handle release() noexcept {
        auto handle = core;
        core = nullptr;
        return handle;
    }

    bool empty() const noexcept {
        return !core;
    }

    bool done() const noexcept {
        return core.done();
    }

    void schedule() {
        core.promise().schedule();
    }

    /// Cancel the task, the suspend point after the current one will be skipped.
    void cancel() {
        core.promise().cancel();
    }

    bool cancelled() {
        return core.promise().cancelled();
    }

    /// Dispose the task, it will be destroyed when finished or cancelled.
    void dispose() {
        core.promise().dispose();
        core = nullptr;
    }

    bool finished() {
        return core.promise().finished();
    }

    T result() {
        if constexpr(!std::is_void_v<T>) {
            return std::move(core.promise().value.value());
        }
    }

    auto operator co_await() const noexcept {
        return awaiter::task<T, promise_type>{core};
    }

    void stacktrace() {
        promise_base* handle = core;
        while(handle) {
            std::println("{}:{}:{}",
                         handle->location.file_name(),
                         handle->location.line(),
                         handle->location.function_name());
            handle = handle->continuation;
        }
    }

private:
    coroutine_handle core;
};

}  // namespace clice::async
