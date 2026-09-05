namespace std {
template <typename T> class initializer_list {
    const T* begin_value;
    const T* end_value;
};
}

namespace binding_placeholder {
struct Dimensions {
    long width;
    double ratio;
};
void inspect() {
    au§(01_structured_bindings)to [width, ratio] = Dimensions{};
}
}

namespace dependent_placeholder {
template <typename T> void inspect() {
    au§(02_undeduced_auto)to value = T{};
}
}

namespace concept_placeholder {
template <typename T>
concept Scalar = true;
Scalar au§(03_constrained_auto)to count = 2L;
}

namespace closure_placeholder {
void inspect() {
    au§(04_auto_lambda)to callback = [](long value) { return value + 1; };
}
}

namespace instance_placeholder {
template <typename T> struct Container {};
void inspect() {
    au§(05_auto_instantiation)to container = Container<long>{};
}
}

namespace specialized_placeholder {
template <typename T> struct Container {};
template <> struct Container<long> {};
void inspect() {
    au§(06_auto_specialized)to container = Container<long>{};
}
}

namespace scalar_placeholder {
§(07_simple_auto)auto value = 21L;
}

namespace const_placeholder {
const §(08_const_auto)auto value = 22L;
}

namespace const_reference_placeholder {
const §(09_const_auto_ref)auto& value = 23L;
}

namespace reference_placeholder {
long value = 24;
§(10_auto_ref)auto& reference = value;
}

namespace pointer_placeholder {
long value = 25;
§(11_auto_ptr)auto* pointer = &value;
}

namespace pointer_value_placeholder {
long value = 26;
§(12_auto_from_pointer)auto pointer = &value;
}

namespace list_placeholder {
§(13_auto_init_list)auto values = {1L, 2L, 3L};
}

namespace conversion_placeholder {
struct Quantity {
    operator §(14_auto_conversion)auto() const { return 27L; }
};
}

namespace trailing_result {
§(15_auto_trailing_return)auto compute() -> long {
    return 28;
}
}

namespace trailing_expression_result {
struct Token {};
§(16_auto_trailing_decltype)auto create() -> decltype(Token{}) {
    return {};
}
}

namespace deduced_result {
struct Token {};
§(17_auto_fn_return)auto create() {
    return Token{};
}
}

namespace reference_result {
struct Token {};
§(18_auto_ref_fn_return)auto& current() {
    static Token token;
    return token;
}
}

namespace pointer_result {
struct Token {};
§(19_auto_ptr_fn_return)auto* current() {
    static Token token;
    return &token;
}
}

namespace const_reference_result {
struct Token {};
const §(20_const_auto_ref_fn_return)auto& current() {
    static Token token;
    return token;
}
}

namespace function_pointer_placeholder {
long compute();
§(21_auto_fn_pointer)auto (*callback)() = compute;
}

namespace scalar_alias_placeholder {
using distance_type = long;
§(22_auto_alias_int)auto distance = distance_type{};
}

namespace record_alias_placeholder {
struct State {};
using state_type = State;
§(23_auto_alias_class)auto state = state_type{};
}

namespace template_instance_placeholder {
template <typename T> struct Wrapper {};
§(24_auto_alias_template)auto wrapper = Wrapper<long>{};
}

namespace dependent_declaration_placeholder {
template <typename T> void inspect() {
    §(25_undeduced_auto_decl)auto value = T{};
}
}

namespace dependent_result_placeholder {
template <typename T> §(26_undeduced_auto_return)auto create() {
    return T{};
}
}

namespace non_type_placeholder {
template <a§(27_template_auto_param)uto Value> void inspect() {}
}
