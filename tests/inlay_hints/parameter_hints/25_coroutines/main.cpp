// `co_yield` calls the promise's `yield_value`, which the code never writes:
// only the calls it does write are hinted.
#include <coroutine>

struct Generator {
    struct promise_type {
        Generator get_return_object();
        std::suspend_always initial_suspend();
        std::suspend_always final_suspend() noexcept;
        std::suspend_always yield_value(int value);
        void return_void();
        void unhandled_exception();
    };
};

int next_value(int seed);

Generator count() {
    co_yield next_value(1);
}
