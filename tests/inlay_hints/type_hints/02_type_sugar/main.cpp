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

template <typename T, typename U = int>
struct Defaulted {};

Defaulted<float> make_defaulted();

void use() {
    auto aliased = make_alias();
    auto shortened = make_compact();
    auto elided = make_long();
    auto mixed = make_mixed();
    auto defaulted = make_defaulted();
}
