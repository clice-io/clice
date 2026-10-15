// auto in the places the documented case leaves out: constrained, of a
// specialization, const reference, initializer list, conversion function,
// trailing decltype, reference and pointer results, function pointer, through
// aliases, template parameter, abbreviated function parameter, new-expression.

namespace std {
template <typename T> class initializer_list {
    const T* begin_value;
    const T* end_value;
};
}

namespace concept_placeholder {
template <typename T>
concept Scalar = true;
Scalar au§(constrained_auto)to count = 2L;
}

namespace specialized_placeholder {
template <typename T> struct Container {};
template <> struct Container<long> {};
void inspect() {
    au§(auto_specialized)to container = Container<long>{};
}
}

namespace const_reference_placeholder {
const §(const_auto_ref)auto& value = 23L;
}

namespace list_placeholder {
§(auto_init_list)auto values = {1L, 2L, 3L};
}

namespace conversion_placeholder {
struct Quantity {
    operator §(auto_conversion)auto() const { return 27L; }
};
}

namespace trailing_expression_result {
struct Token {};
§(auto_trailing_decltype)auto create() -> decltype(Token{}) {
    return {};
}
}

namespace reference_result {
struct Token {};
§(auto_ref_fn_return)auto& current() {
    static Token token;
    return token;
}
}

namespace pointer_result {
struct Token {};
§(auto_ptr_fn_return)auto* current() {
    static Token token;
    return &token;
}
}

namespace const_reference_result {
struct Token {};
const §(const_auto_ref_fn_return)auto& current() {
    static Token token;
    return token;
}
}

namespace function_pointer_placeholder {
long compute();
§(auto_fn_pointer)auto (*callback)() = compute;
}

namespace scalar_alias_placeholder {
using distance_type = long;
§(auto_alias_int)auto distance = distance_type{};
}

namespace record_alias_placeholder {
struct State {};
using state_type = State;
§(auto_alias_class)auto state = state_type{};
}

namespace template_instance_placeholder {
template <typename T> struct Wrapper {};
§(auto_alias_template)auto wrapper = Wrapper<long>{};
}

namespace dependent_result_placeholder {
template <typename T> §(undeduced_auto_return)auto create() {
    return T{};
}
}

namespace non_type_placeholder {
template <a§(template_auto_param)uto Value> void inspect() {}
}

namespace abbreviated_placeholder {
void inspect(au§(abbreviated_param)to value) {}

void use() {
    inspect(29L);
}
}

namespace expression_placeholder {
long* allocated = new au§(new_auto)to(35L);
}
