namespace dependent_array_bound {

template <typename T>
struct trait {
    using type = void;
};

template <typename T, unsigned long N>
struct trait<T[N]> {
    using type = T;
};

template <typename X, unsigned long M>
struct test {
    using §(dependent_array_bound)input = typename trait<X[M]>::type;
};

}  // namespace dependent_array_bound

namespace unbound_array_pattern {

template <typename T>
struct trait {
    using type = void;
};

template <typename T>
struct trait<T[]> {
    using type = T;
};

template <typename X>
struct test {
    using §(unbound_array_pattern)input = typename trait<X[]>::type;
};

}  // namespace unbound_array_pattern

namespace const_method_pattern {

template <typename T>
struct trait {
    using type = void;
};

template <typename R, typename C, typename... As>
struct trait<R (C::*)(As...) const> {
    using type = R;
};

template <typename X>
struct test {
    // Not a const member function: the primary's `void`.
    using §(const_method_pattern)input = typename trait<int (X::*)(char)>::type;
};

}  // namespace const_method_pattern

namespace noexcept_function_pattern {

template <typename T>
struct trait {
    using type = void;
};

template <typename R, typename... As>
struct trait<R(As...) noexcept> {
    using type = R;
};

template <typename X>
struct test {
    // Not `noexcept`: the primary's `void`.
    using §(noexcept_function_pattern)input = typename trait<X(int)>::type;
};

}  // namespace noexcept_function_pattern

namespace function_type_pattern {

template <typename T>
struct trait {
    using type = void;
};

template <typename R, typename... As>
struct trait<R(As...)> {
    using type = R;
};

template <typename X>
struct test {
    using §(function_type_pattern)input = typename trait<X(int)>::type;
};

}  // namespace function_type_pattern

namespace member_pointer_pattern {

template <typename T>
struct trait {
    using type = void;
};

template <typename R, typename C>
struct trait<R C::*> {
    using type = R;
};

template <typename X>
struct test {
    using §(member_pointer_pattern)input = typename trait<int X::*>::type;
};

}  // namespace member_pointer_pattern

namespace bool_array_bound {

template <typename T>
struct trait {
    using type = void;
};

template <bool N, typename U>
struct trait<U[N]> {
    using type = U;
};

template <typename X>
struct test {
    // `bool N` cannot hold the bound 2: the primary's `void`.
    using §(bool_array_bound)input = typename trait<X[2]>::type;
};

}  // namespace bool_array_bound

namespace unbounded_argument {

template <int N, typename B>
struct P {
    using type = void;
};

template <int N, typename U>
struct P<N, U[N]> {
    using type = U;
};

template <typename X>
struct test {
    // `U[N]` has a bound and `X[]` none: the primary's `void`.
    using §(unbounded_argument)input = typename P<3, X[]>::type;
};

}  // namespace unbounded_argument

namespace auto_array_bound {

template <typename T>
struct trait {
    using type = void;
};

template <typename U, auto N>
struct trait<U[N]> {
    using type = U;
};

template <typename X>
struct test {
    using §(auto_array_bound)input = typename trait<X[3]>::type;
};

}  // namespace auto_array_bound

namespace noexcept_deduce {

template <bool B>
struct flag {};

template <typename T>
struct trait {
    using type = void;
};

template <bool B>
struct trait<void() noexcept(B)> {
    using type = flag<B>;
};

template <typename X, bool P>
struct test {
    using §(noexcept_deduce)input = typename trait<void() noexcept(P)>::type;
};

}  // namespace noexcept_deduce

namespace repeated_parameter_undecided {

template <typename T, typename U>
struct same {
    using type = char;
};

template <typename T>
struct same<T, T> {
    using type = int;
};

template <typename K, typename V>
struct test {
    // `K` and `V` may be one type: either pattern may apply.
    using §(repeated_parameter_undecided)input = typename same<K, V>::type;
};

}  // namespace repeated_parameter_undecided

namespace repeated_parameter_enclosing {

template <typename T, typename U>
struct same {
    using type = char;
};

template <typename T>
struct same<T, T> {
    using type = int;
};

template <typename A, typename B>
struct pair {};

template <typename K, typename V>
struct test {
    // A type never holds itself: `K` is no `pair<const K, V>`.
    using §(repeated_parameter_enclosing)input = typename same<K, pair<const K, V>>::type;
};

}  // namespace repeated_parameter_enclosing
