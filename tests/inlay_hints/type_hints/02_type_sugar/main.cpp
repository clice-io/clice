using Integer = int;

Integer make_alias();

template <typename A, typename B, typename C>
struct extremely_long_template_name {};

using Compact = extremely_long_template_name<int, char, bool>;

Compact make_compact();

extremely_long_template_name<Integer, Integer, Integer> make_long();

template <typename... Ts>
struct tuple {};

tuple<int, double, extremely_long_template_name<int, char, bool>> make_mixed();

template <typename A, typename B>
struct Pair {};

tuple<int, Pair<long, extremely_long_template_name<int, char, bool>>> make_nested();

template <typename T>
struct an_extraordinarily_long_class_name {};

an_extraordinarily_long_class_name<int> make_far();

template <typename F>
struct function {};

function<auto(const char*)->bool> make_callback();

template <typename T, typename U = int>
struct Defaulted {};

Defaulted<float> make_defaulted();

void use() {
    auto aliased = make_alias();
    auto shortened = make_compact();
    auto elided = make_long();
    auto mixed = make_mixed();
    auto nested = make_nested();
    auto dropped = make_far();
    auto callback = make_callback();
    auto defaulted = make_defaulted();
}
