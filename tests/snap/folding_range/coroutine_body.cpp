/// # Fold Kinds
///
/// ## Coroutine bodies — the written block folds exactly once and the coroutine transformation wrapper adds no duplicate fold; a coroutine lambda keeps its body fold
///
/// - status: supported
/// - order: 15

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

Task work() {
    int steps = 0;
    if (steps == 0) {
        steps += 1;
    }
    co_return;
}

void host() {
    auto nested = []() -> Task {
        int steps = 0;
        steps += 1;
        co_return;
    };
}
