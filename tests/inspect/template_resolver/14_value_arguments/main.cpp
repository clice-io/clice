namespace nttp_passthrough {

template <typename T, int N>
struct box {};

template <typename T, int N>
struct S {
    using type = box<T, N>;
};

template <typename X>
struct test {
    using §(nttp_passthrough)input = typename S<X, 5>::type;
};

}  // namespace nttp_passthrough

namespace nttp_bool_value {

template <typename T, bool B>
struct box {};

template <typename T, bool B>
struct S {
    using type = box<T, B>;
};

template <typename X>
struct test {
    using §(nttp_bool_value)input = typename S<X, true>::type;
};

}  // namespace nttp_bool_value

namespace nttp_enum_value {

enum Color { Red, Green, Blue };

template <typename T, Color C>
struct box {};

template <typename T, Color C>
struct S {
    using type = box<T, C>;
};

template <typename X>
struct test {
    using §(nttp_enum_value)input = typename S<X, Green>::type;
};

}  // namespace nttp_enum_value

namespace repeated_value_pattern {

template <int A, int B>
struct P {
    using type = void;
};

template <int N>
struct P<N, N> {
    using type = int;
};

template <int X>
struct test {
    using §(repeated_value_pattern)input = typename P<X, X>::type;
};

}  // namespace repeated_value_pattern

namespace typed_value_mismatch {

template <auto V>
struct A {
    using type = void;
};

template <bool B>
struct A<B> {
    using type = int;
};

template <int X>
struct test {
    // A `bool` parameter never matches an `int` argument: the primary's `void`.
    using §(typed_value_mismatch)input = typename A<X>::type;
};

}  // namespace typed_value_mismatch

namespace auto_value_type {

template <auto A, auto B>
struct trait {
    using type = void;
};

template <auto B>
struct trait<1, B> {
    using type = int;
};

template <auto Y>
struct test {
    // `1L` is a `long`, the partial's `1` an `int`: the primary's `void`.
    using §(auto_value_type)input = typename trait<1L, Y>::type;
};

}  // namespace auto_value_type

namespace dependent_typed_value {

template <typename T, T N>
struct A {
    using type = T;
};

template <typename X>
struct test {
    using §(dependent_typed_value)input = typename A<X, 1>::type;
};

}  // namespace dependent_typed_value

namespace compound_value_match {

template <int A, int B>
struct trait {
    using type = void;
};

template <int N>
struct trait<N, N + 1> {
    using type = int;
};

template <int X>
struct test {
    using §(compound_value_match)input = typename trait<X, X + 1>::type;
};

}  // namespace compound_value_match

namespace compound_value_unverified {

template <int A, int B>
struct trait {
    using type = void;
};

template <int N>
struct trait<N, N + 1> {
    using type = int;
};

template <int X>
struct test {
    // `X + 2` is not provably `X + 1`: neither pattern applies, it stays dependent.
    using §(compound_value_unverified)input = typename trait<X, X + 2>::type;
};

}  // namespace compound_value_unverified

namespace compound_value_concrete {

template <int A, int B>
struct trait {
    using type = void;
};

template <int N>
struct trait<N, N + 1> {
    using type = int;
};

template <typename T, int A, int B>
struct apply {
    using type = typename trait<A, B>::type;
};

template <typename X>
struct test {
    // 5 is not 3 + 1: the primary's `void`.
    using §(compound_value_concrete)input = typename apply<X, 3, 5>::type;
};

}  // namespace compound_value_concrete

namespace compound_value_argument {

template <int N>
struct Arr {
    using type = char;
};

template <>
struct Arr<4> {
    using type = int;
};

template <class T, int N>
struct Outer {
    using inner = typename Arr<N + 1>::type;
};

template <typename X>
struct test {
    // `N + 1` folds to 4: the explicit specialization's `int`.
    using §(compound_value_argument)input = typename Outer<X, 3>::inner;
};

}  // namespace compound_value_argument
