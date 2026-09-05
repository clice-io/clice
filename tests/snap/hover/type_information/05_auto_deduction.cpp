/// # `auto` deduction
///
/// - status: supported
///
/// Placeholder hover shows the type it resolves to
///
/// Hovering an `auto` placeholder shows the type substituted for it —
/// builtins, pointers, lambdas, template instantiations, and the
/// `/* not deduced */` marker inside an uninstantiated template.

namespace placeholder_types {
struct Result {};
struct Dimensions {
    long rows;
    long columns;
};
template <typename T> struct Holder {};

void locals() {
    long source = 0;
    §(01_simple)auto first = 4L;
    const §(02_const)auto second = 5L;
    §(03_ref)auto& third = source;
    §(04_ptr)auto* fourth = &source;
    §(05_from_pointer)auto fifth = &source;
    §(06_lambda)auto sixth = [](long value) { return value; };
    §(07_instantiation)auto seventh = Holder<long>{};
    §(08_structured)auto [rows, columns] = Dimensions{};
}

§(09_trailing_return)auto explicit_result() -> long {
    return 6;
}

§(10_fn_return)auto inferred_result() {
    return Result{};
}

template <typename T> void pending() {
    §(11_undeduced)auto value = T{};
}
}
