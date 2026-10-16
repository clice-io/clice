#include "coroutine.h"

template <class T>
struct Generator {
    struct promise_type {
        Generator get_return_object();
        std::suspend_always initial_suspend();
        std::suspend_always final_suspend() noexcept;
        std::suspend_always yield_value(T value);
        void return_void();
        void unhandled_exception();
    };
};

template <class T>
Generator<T> produce(T value) {
    §(co_yield)co_yield value;
    §(co_await)co_await std::suspend_always{};
}
