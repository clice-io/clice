// decltype in the places the documented case leaves out: const references,
// deduced results, references and rvalue calls, decltype of decltype, a
// variable declared through decltype, a signature spelled with it.

namespace const_reference_placeholder {
long source = 33;
const long& reference = source;
decl§(const_ref_decltype_auto)type(auto) value = reference;
}

namespace deduced_result {
struct Message {};
decl§(decltype_auto_fn_return)type(auto) create() {
    return Message{};
}
}

namespace reference_result {
decl§(decltype_auto_ref_return)type(auto) current() {
    static long value;
    return (value);
}
}

namespace declared_reference {
long source;
long& reference = source;
decl§(decltype_lvalue_ref)type(reference) alias = source;
}

namespace rvalue_call {
long&& acquire();
decl§(decltype_rvalue_call)type(acquire()) moved = acquire();
}

namespace trailing_return_call {
struct Message {};
auto create() -> decltype(Message{}) {
    return {};
}
decl§(decltype_trailing_return)type(create()) message = create();
}

namespace chained_declaration {
long source;
decltype(source) intermediate;
decl§(decltype_of_decltype)type(intermediate) copy = source;
}

namespace variable_type_chain {
long source;
decltype(source) intermediate;
decltype(intermediate) §(var_decltype_chain)copy = source;
}

namespace const_variable_type {
long source;
const decltype(source) §(var_const_decltype)copy = source;
}

namespace signature_type {
long source;
auto §(fn_decltype_signature)convert(decltype(source) value) -> decltype(source) {
    return value;
}
}

namespace inner_placeholder {
decltype(au§(decltype_auto_inner)to) value = 34L;
}
