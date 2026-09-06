namespace direct_placeholder {
decl§(01_decltype_auto)type(auto) value = 31L;
}

namespace const_placeholder {
const long source = 32;
decl§(02_const_decltype_auto)type(auto) value = source;
}

namespace const_reference_placeholder {
long source = 33;
const long& reference = source;
decl§(03_const_ref_decltype_auto)type(auto) value = reference;
}

namespace reference_placeholder {
long source = 34;
long& reference = source;
decl§(04_ref_decltype_auto)type(auto) value = reference;
}

namespace deduced_result {
struct Message {};
decl§(05_decltype_auto_fn_return)type(auto) create() {
    return Message{};
}
}

namespace reference_result {
decl§(06_decltype_auto_ref_return)type(auto) current() {
    static long value;
    return (value);
}
}

namespace declared_lvalue {
long source;
decl§(07_decltype_lvalue)type(source) copy = source;
}

namespace declared_reference {
long source;
long& reference = source;
decl§(08_decltype_lvalue_ref)type(reference) alias = source;
}

namespace parenthesized_lvalue {
long source;
decl§(09_decltype_paren_lvalue)type((source)) alias = source;
}

namespace rvalue_reference {
long source;
decl§(10_decltype_rvalue_ref)type(static_cast<long&&>(source)) moved =
    static_cast<long&&>(source);
}

namespace rvalue_call {
long&& acquire();
decl§(11_decltype_rvalue_call)type(acquire()) moved = acquire();
}

namespace trailing_return_call {
struct Message {};
auto create() -> decltype(Message{}) {
    return {};
}
decl§(12_decltype_trailing_return)type(create()) message = create();
}

namespace chained_declaration {
long source;
decltype(source) intermediate;
decl§(13_decltype_of_decltype)type(intermediate) copy = source;
}

namespace dependent_expression {
template <typename T> struct MemberType {
    using type = decl§(14_decltype_dependent)type(T::member);
};
}

namespace dependent_result {
template <typename T> decl§(15_undeduced_decltype_auto)type(auto) create() {
    return T{};
}
}

namespace variable_type {
long source;
decltype(source) §(16_var_decltype_type)copy = source;
}

namespace variable_type_chain {
long source;
decltype(source) intermediate;
decltype(intermediate) §(17_var_decltype_chain)copy = source;
}

namespace const_variable_type {
long source;
const decltype(source) §(18_var_const_decltype)copy = source;
}

namespace signature_type {
long source;
auto §(19_fn_decltype_signature)convert(decltype(source) value) -> decltype(source) {
    return value;
}
}
