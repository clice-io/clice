/// # Implicit Code Navigation
///
/// ## `co_await` / `co_yield` / `co_return` — navigate to the awaiter or promise method
///
/// - status: partial
/// - verify: server
/// - order: 21
///
/// Go-to-definition on `co_yield` reaches the promise's `yield_value`. The
/// `co_await` and `co_return` keywords do not yet reach the awaiter's or
/// promise's methods.

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
}

struct Awaiter {
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<>) const noexcept;
    int await_resume() const noexcept;
};

struct Task {
    struct promise_type {
        Task get_return_object();
        std::suspend_never initial_suspend();
        std::suspend_never final_suspend() noexcept;
        Awaiter yield_value(int value);
        void return_value(int value);
        void unhandled_exception();
    };
};

Task example() {
    §(co_await_kw)co_await Awaiter{};
    §(co_yield_kw)co_yield 1;
    §(co_return_kw)co_return 2;
}
