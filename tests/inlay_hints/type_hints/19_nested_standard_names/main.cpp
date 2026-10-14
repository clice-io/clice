#include <chrono>

template <typename T>
struct Box {
    T get();
};

void use(Box<std::chrono::seconds> timeouts,
         Box<std::chrono::duration<long, std::ratio<1, 1000>>> delays,
         Box<std::chrono::duration<int, std::milli>> ticks) {
    auto timeout = timeouts.get();
    auto delay = delays.get();
    auto tick = ticks.get();
}
