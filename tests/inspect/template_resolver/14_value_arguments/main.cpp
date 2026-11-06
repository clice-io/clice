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

namespace unknown_flag {

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename X>
struct test {
    // Whether `X` is wider than 4 bytes waits for instantiation: either pattern may apply, and
    // their types differ.
    using §(unknown_flag)input = typename conditional<(sizeof(X) > 4), int, char>::type;
};

}  // namespace unknown_flag

namespace unknown_flag_one_pattern {

template <bool B, typename T>
struct enable_if {};

template <typename T>
struct enable_if<true, T> {
    using type = T;
};

template <typename X>
struct test {
    // Only the partial declares `type`: the code naming it instantiates with the flag true.
    using §(unknown_flag_one_pattern)input = typename enable_if<(sizeof(X) > 4), X*>::type;
};

}  // namespace unknown_flag_one_pattern

namespace unknown_flag_explicit {

template <bool>
struct select {
    template <typename T, typename F>
    using type = T;
};

template <>
struct select<false> {
    template <typename T, typename F>
    using type = F;
};

template <typename X>
struct test {
    // The explicit specialization for `false` may apply as well.
    using §(unknown_flag_explicit)input =
        typename select<(sizeof(X) > 4)>::template type<int, char>;
};

}  // namespace unknown_flag_explicit

namespace unknown_wide_value {

template <typename T, int N>
struct array {
    using type = T;
};

template <typename T>
struct array<T, 0> {
    using type = void;
};

template <typename X, int N>
struct test {
    // One value of an `int` is a special case: with `N` not known, the primary's `T`.
    using §(unknown_wide_value)input = typename array<X, N>::type;
};

}  // namespace unknown_wide_value

namespace variable_template_value {

template <typename T, typename U>
constexpr bool same = false;

template <typename T>
constexpr bool same<T, T> = true;

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename T>
struct box {};

template <typename K>
struct test {
    // `K` is never the `box<K>` holding it: `same` is false.
    using §(variable_template_value)input = typename conditional<same<K, box<K>>, int, char>::type;
};

}  // namespace variable_template_value

namespace variable_template_undecided {

template <typename T, typename U>
constexpr bool same = false;

template <typename T>
constexpr bool same<T, T> = true;

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename K, typename V>
struct test {
    // `K` and `V` may be one type.
    using §(variable_template_undecided)input = typename conditional<same<K, V>, int, char>::type;
};

}  // namespace variable_template_undecided

namespace builtin_same {

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename K>
struct test {
    using §(builtin_same)input = typename conditional<__is_same(K, const K*), int, char>::type;
};

template <typename K>
struct test_reference {
    // `K = int&` makes `K&` the type `K` is.
    using §(builtin_same_reference)input = typename conditional<__is_same(K, K&), int, char>::type;
};

template <typename T>
struct test_collapse {
    // `T = int&` makes `T&&` an `int&`.
    using §(builtin_same_collapse)input = typename conditional<__is_same(T&&, int&), int, char>::type;
};

template <int N>
struct test_extent {
    // `N = 3` makes the two one type.
    using §(builtin_same_extent)input = typename conditional<__is_same(int[N], int[3]), int, char>::type;
};

template <typename T>
struct box {};

template <template <typename> class TT>
struct test_template {
    // `TT = box` makes the two one type.
    using §(builtin_same_template)input =
        typename conditional<__is_same(TT<int>, box<int>), int, char>::type;
};

}  // namespace builtin_same

namespace unused_member_value {

template <typename T, T V>
struct constant {
    static constexpr T value = V;
};

// The specialization is instantiated, its `value` initializer only once a use needs it.
using no = constant<bool, false>;
no instance;

template <typename A, typename B>
struct same : constant<bool, __is_same(A, B)> {};

template <typename T>
struct allocator {
    using rebound = allocator;
};

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename K>
struct test {
    using §(unused_member_value)input = typename conditional<same<K, K*>::value, int, char>::type;
    // `rebound` names the allocator itself.
    using §(resolved_trait_operand)rebound =
        typename conditional<same<typename allocator<K>::rebound, allocator<K>>::value, int, char>::type;
};

}  // namespace unused_member_value

namespace member_flag {

template <typename T, typename U>
constexpr bool same = false;

template <typename T>
constexpr bool same<T, T> = true;

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename A, typename B>
struct pair {};

template <typename K, typename V>
struct traits {
    using key_type = K;
    using value_type = pair<const K, V>;
};

template <typename Traits>
struct tree {
    using key_type = typename Traits::key_type;
    using value_type = typename Traits::value_type;
    static constexpr bool is_set = same<key_type, value_type>;

    struct const_iterator {};

    struct mutable_iterator {};

    using iterator = typename conditional<is_set, const_iterator, mutable_iterator>::type;
};

template <typename K, typename V>
struct test {
    // A map's keys are never its values: the mutable iterator.
    using §(member_flag)input = typename tree<traits<K, V>>::iterator*;
};

}  // namespace member_flag

namespace explicit_variable_specialization {

template <bool B, typename T, typename F>
struct conditional {
    using type = T;
};

template <typename T, typename F>
struct conditional<false, T, F> {
    using type = F;
};

template <typename T>
constexpr bool enabled = false;

template <>
constexpr bool enabled<int> = true;

template <typename T>
struct Source {
    using type = int;
};

template <typename T>
struct test {
    // `enabled<int>` is its explicit specialization's.
    using §(explicit_variable_specialization)input =
        typename conditional<enabled<typename Source<T>::type>, int, char>::type;
};

template <typename... Ts>
constexpr bool any_enabled = false;

template <>
constexpr bool any_enabled<int> = true;

template <typename T>
struct test_pack {
    // Clang holds the specialization's arguments as one pack.
    using §(explicit_variadic_specialization)input =
        typename conditional<any_enabled<typename Source<T>::type>, int, char>::type;
};

}  // namespace explicit_variable_specialization
