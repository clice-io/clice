// Deduced type hints: sugar preservation vs desugaring, the length-limit
// fallback to the sugared spelling, structured bindings (canonical types),
// lambdas, auto parameters with a single instantiation, and auto return
// types.

namespace basic_auto {

int make();

void use() {
    auto value = make();
    const auto& ref = value;
    auto* ptr = &value;
}

}  // namespace basic_auto

namespace alias_desugar {

using Integer = int;

Integer make();

void use() {
    auto value = make();
}

}  // namespace alias_desugar

namespace long_type_fallback {

template <typename A, typename B, typename C>
struct extremely_long_template_name {};

using Compact = extremely_long_template_name<int, char, bool>;

Compact make();

void use() {
    auto value = make();
}

}  // namespace long_type_fallback

namespace bindings {

struct Pair {
    int first;
    float second;
};

Pair make();

void use() {
    auto [a, b] = make();
}

}  // namespace bindings

namespace lambdas {

void use() {
    auto callback = [](int x) {
        return x;
    };
}

}  // namespace lambdas

namespace auto_params {

int twice(auto x) {
    return x + x;
}

int result = twice(21);

}  // namespace auto_params

namespace return_types {

auto answer() {
    return 42;
}

auto& ref_answer() {
    static int storage = 0;
    return storage;
}

}  // namespace return_types
