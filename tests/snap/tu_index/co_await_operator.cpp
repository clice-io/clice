// - verify: inspect
//
// The co_await keyword references the operator co_await it selects.

namespace std {
template <typename Ret, typename...>
struct coroutine_traits {
    using promise_type = typename Ret::promise_type;
};

template <typename = void>
struct coroutine_handle {
    coroutine_handle() = default;
    template <typename Promise>
    coroutine_handle(coroutine_handle<Promise>) noexcept;
    static coroutine_handle from_address(void*) noexcept;
};

struct suspend_never {
    bool await_ready() const noexcept;
    void await_suspend(coroutine_handle<>) const noexcept;
    void await_resume() const noexcept;
};
}  // namespace std

struct Task {
    struct promise_type {
        Task get_return_object();
        std::suspend_never initial_suspend();
        std::suspend_never final_suspend() noexcept;
        void return_void();
        void unhandled_exception();
    };
};

struct Awaitable {
    std::suspend_never operator co_await() const;
};

Task coro(Awaitable value) {
    co_await value;
}
