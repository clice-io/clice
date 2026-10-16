#pragma once

namespace fmt {

template <class T>
struct type_identity {
    using type = T;
};

template <class... Args>
struct basic_format_string {
    consteval basic_format_string(const char* text) : text(text) {}

    const char* text;
};

template <class... Args>
using format_string = basic_format_string<typename type_identity<Args>::type...>;

template <class... Args>
void print(format_string<Args...> format, Args&&... args) {}

}  // namespace fmt

[[gnu::format(printf, 1, 2)]] int log_line(const char* format, ...);

struct Logger {
    [[gnu::format(printf, 2, 3)]] void log(const char* format, ...);
    [[gnu::format(printf, 2, 3)]] void operator()(const char* format, ...);
};
