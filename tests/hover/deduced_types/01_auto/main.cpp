namespace placeholder_types {
struct Result {};
struct Dimensions {
    long rows;
    long columns;
};
template <typename T> struct Holder {};

void locals() {
    long source = 0;
    §(simple)auto first = 4L;
    const §(const)auto second = 5L;
    §(ref)auto& third = source;
    §(ptr)auto* fourth = &source;
    §(from_pointer)auto fifth = &source;
    §(lambda)auto sixth = [](long value) { return value; };
    §(instantiation)auto seventh = Holder<long>{};
    §(structured)auto [rows, columns] = Dimensions{};
}

§(trailing_return)auto explicit_result() -> long {
    return 6;
}

§(fn_return)auto inferred_result() {
    return Result{};
}

template <typename T> void pending() {
    §(undeduced)auto value = T{};
}
}
