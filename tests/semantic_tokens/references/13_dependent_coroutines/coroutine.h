#pragma once

namespace std {

template <class Result, class... Args>
struct coroutine_traits {
    using promise_type = typename Result::promise_type;
};

template <class Promise = void>
struct coroutine_handle;

template <>
struct coroutine_handle<void> {
    static coroutine_handle from_address(void* address) noexcept;
};

template <class Promise>
struct coroutine_handle {
    static coroutine_handle from_address(void* address) noexcept;
    operator coroutine_handle<>() const noexcept;
};

struct suspend_always {
    bool await_ready() noexcept {
        return false;
    }

    void await_suspend(coroutine_handle<>) noexcept {}

    void await_resume() noexcept {}
};

}  // namespace std
