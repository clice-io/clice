namespace partial_default_argument {

template <typename T, typename U = T>
struct X {};

template <typename T>
struct X<T, T> {
    using type = T;
};

template <typename T>
struct test {
    using §(partial_default_argument)input = typename X<T>::type;
};

}  // namespace partial_default_argument

namespace default_argument {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = type_list<T1>;
};

template <typename U1, typename U2 = A<U1>>
struct B {
    using type = typename U2::type;
};

template <typename X>
struct test {
    using §(default_argument)input = typename B<X>::type;
};

}  // namespace default_argument

namespace nttp_default_argument {

template <typename T, int N = 0>
struct S {
    using type = T;
};

template <typename X>
struct test {
    using §(nttp_default_argument)input = typename S<X>::type;
};

}  // namespace nttp_default_argument

namespace default_arg_earlier_param {

template <typename T, typename U = T*>
struct A {
    using type = U;
};

template <typename X>
struct test {
    using §(default_arg_earlier_param)input = typename A<X>::type;
};

}  // namespace default_arg_earlier_param

namespace pack_default_interplay {

template <typename... Ts>
struct type_list {};

template <typename T, typename U = int, typename... Vs>
struct A {
    using type = type_list<T, U, Vs...>;
};

template <typename X>
struct test {
    using §(pack_default_interplay)input = typename A<X>::type;
};

}  // namespace pack_default_interplay

namespace inherited_default_argument {

template <typename T, typename U = T>
struct A;

template <typename T, typename U>
struct A {
    using type = U;
};

template <typename X>
struct test {
    using §(inherited_default_argument)input = typename A<X>::type;
};

}  // namespace inherited_default_argument

namespace alias_default_argument {

template <typename T, typename U>
struct pair {};

template <typename T, typename U = int>
using alias_pair = pair<T, U>;

template <template <typename> class TT, typename A>
struct apply {
    using type = TT<A>;
};

template <typename X>
struct test {
    using §(alias_default_argument)input = typename apply<alias_pair, X>::type;
};

}  // namespace alias_default_argument

namespace defaults_in_canonical {

template <typename T, typename U = int>
struct target {};

template <template <typename> class TT, typename A>
struct apply {
    using type = TT<A>;
};

template <typename X>
struct test {
    using §(defaults_in_canonical)input = typename apply<target, X>::type;
};

}  // namespace defaults_in_canonical

namespace dependent_value_default {

template <int N>
struct value {};

template <int N, int M = N>
struct A {
    using type = value<M>;
};

template <int X>
struct test {
    using §(dependent_value_default)input = typename A<X>::type;
};

}  // namespace dependent_value_default

namespace promoted_dependent_value {

template <typename T>
struct Settings {
    static constexpr unsigned char n = 1;
};

template <typename T, int N = -Settings<T>::n>
struct P {
    using type = char;
};

template <typename T>
struct P<T, -1> {
    using type = int;
};

template <typename X>
struct test {
    // `-n` promotes to `int`: -1, the partial's `int`.
    using §(promoted_dependent_value)input = typename P<X>::type;
};

}  // namespace promoted_dependent_value

namespace dependent_conditional_value {

template <typename T>
struct Settings {
    static constexpr bool flag = true;
};

template <typename T, long long N = (Settings<T>::flag ? -1 : 0u)>
struct P {
    using type = char;
};

template <typename T>
struct P<T, -1> {
    using type = int;
};

template <typename X>
struct test {
    // `-1` meets `0u` as unsigned: the default is not -1, so not the partial's `int`.
    using §(dependent_conditional_value)input = typename P<X>::type;
};

}  // namespace dependent_conditional_value

namespace signed_overflow_value {

template <typename T>
struct Settings {
    static constexpr int top = 2147483647;
};

template <typename T, int N = Settings<T>::top + 1>
struct P {
    using type = char;
};

template <typename T>
struct P<T, -2147483647 - 1> {
    using type = int;
};

template <typename X>
struct test {
    // The default overflows: no constant, so not the partial's `int`.
    using §(signed_overflow_value)input = typename P<X>::type;
};

}  // namespace signed_overflow_value

namespace member_value_default {

template <typename T>
struct Settings {
    static const int stages = 2;
};

template <typename A, int S = Settings<A>::stages>
struct Gemm {
    using type = void;
};

template <typename A>
struct Gemm<A, 2> {
    using type = int;
};

template <typename X>
struct test {
    using §(member_value_default)input = typename Gemm<X>::type;
};

}  // namespace member_value_default

namespace dependent_typed_member_value {

template <typename Tag, typename I = unsigned char>
struct Id {
    static constexpr I invalid = -1;
};

template <typename T, int N = Id<T>::invalid>
struct P {
    using type = char;
};

template <typename T>
struct P<T, -1> {
    using type = int;
};

template <typename T>
struct P<T, 255> {
    using type = long;
};

template <typename X>
struct test {
    // `invalid` is an `unsigned char`: 255, not -1.
    using §(dependent_typed_member_value)input = typename P<X>::type;
};

}  // namespace dependent_typed_member_value

namespace mutable_value_default {

template <typename T>
struct Settings {
    static inline int stages = 2;
};

template <typename A, int S = Settings<A>::stages>
struct Gemm {
    using type = void;
};

template <typename A>
struct Gemm<A, 2> {
    using type = int;
};

template <typename X>
struct test {
    // A non-const static member is no constant: the default is unknown, it stays dependent.
    using §(mutable_value_default)input = typename Gemm<X>::type;
};

}  // namespace mutable_value_default
