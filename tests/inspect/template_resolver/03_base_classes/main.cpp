namespace base_dependent {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = type_list<T1>;
};

template <typename U2>
struct B : A<U2> {};

template <typename X>
struct test {
    using §(base_dependent)input = typename B<X>::type;
};

}  // namespace base_dependent

namespace base_pack_expansion {

template <typename... Ts>
struct type_list {};

template <typename U, typename... Us>
struct X {
    using type = type_list<Us...>;
};

template <typename... Us>
struct Y : X<int, Us...> {};

template <typename... Ts>
struct test {
    using §(base_pack_expansion)input = typename Y<Ts...>::type;
};

}  // namespace base_pack_expansion

namespace recursive_base_class {

template <typename F>
struct callback_traits : callback_traits<decltype(&F::operator())> {};

template <typename R, typename C, typename... Args>
struct callback_traits<R (C::*)(Args...) const> {
    using result_type = R;
};

template <typename F>
struct test {
    // The base names itself through `decltype`: resolving ends, the name stays dependent.
    using §(recursive_base_class)input = typename callback_traits<F>::result_type;
};

}  // namespace recursive_base_class

namespace multiple_inheritance {

template <typename... Ts>
struct type_list {};

template <typename T>
struct Base1 {
    using type1 = type_list<T>;
};

template <typename T>
struct Base2 {
    using type2 = T;
};

template <typename T>
struct Derived : Base1<T>, Base2<T> {};

template <typename X>
struct test {
    using §(multiple_inheritance)input = typename Derived<X>::type1;
};

}  // namespace multiple_inheritance

namespace second_base_inheritance {

template <typename T>
struct Base1 {
    using type1 = int;
};

template <typename T>
struct Base2 {
    using type2 = T;
};

template <typename T>
struct Derived : Base1<T>, Base2<T> {};

template <typename X>
struct test {
    using §(second_base_inheritance)input = typename Derived<X>::type2;
};

}  // namespace second_base_inheritance

namespace crtp_pattern {

template <typename Derived>
struct Base {
    using derived_type = Derived;
};

template <typename T>
struct Impl : Base<Impl<T>> {
    using type = T;
};

template <typename X>
struct test {
    using §(crtp_pattern)input = typename Impl<X>::type;
};

}  // namespace crtp_pattern

namespace indirect_base_class {

template <typename T>
struct GrandBase {
    using type = T;
};

template <typename T>
struct Middle : GrandBase<T> {};

template <typename T>
struct Top : Middle<T> {};

template <typename X>
struct test {
    using §(indirect_base_class)input = typename Top<X>::type;
};

}  // namespace indirect_base_class

namespace non_dependent_base {

struct Base {
    using type = int;
};

template <typename T>
struct Derived : Base {};

template <typename X>
struct test {
    using §(non_dependent_base)input = typename Derived<X>::type;
};

}  // namespace non_dependent_base

namespace three_level_inheritance {

template <typename... Ts>
struct type_list {};

template <typename T>
struct L1 {
    using type = type_list<T>;
};

template <typename T>
struct L2 : L1<T> {};

template <typename T>
struct L3 : L2<T> {};

template <typename T>
struct L4 : L3<T> {};

template <typename X>
struct test {
    using §(three_level_inheritance)input = typename L4<X>::type;
};

}  // namespace three_level_inheritance

namespace partial_member_via_base {

template <typename... Ts>
struct type_list {};

template <typename T>
struct holder {
    using type = type_list<T>;
};

template <typename T>
struct picker {
    using type = int;
};

template <typename T>
struct picker<T*> : holder<T> {};

template <typename X>
struct test {
    using §(partial_member_via_base)input = typename picker<X*>::type;
};

}  // namespace partial_member_via_base

namespace crtp_partial_spec {

template <typename... Ts>
struct type_list {};

template <typename D>
struct facade {
    using type = typename D::value;
};

template <typename T>
struct widget;

template <typename T>
struct widget<T*> : facade<widget<T*>> {
    using value = type_list<T>;
};

template <typename X>
struct test {
    using §(crtp_partial_spec)input = typename widget<X*>::type;
};

}  // namespace crtp_partial_spec

namespace inheritance_through_partial {

template <typename... Ts>
struct type_list {};

template <typename T>
struct groot {
    using type = type_list<T>;
};

template <typename T>
struct mid {
    using type = int;
};

template <typename T>
struct mid<T*> : groot<T> {};

template <typename T>
struct topc : mid<T*> {};

template <typename X>
struct test {
    using §(inheritance_through_partial)input = typename topc<X>::type;
};

}  // namespace inheritance_through_partial

namespace ambiguous_base_member {

template <typename T>
struct Left {
    using type = int;
};

template <typename T>
struct Right {
    using type = char;
};

template <typename T>
struct Combined : Left<T>, Right<T> {};

template <typename X>
struct test {
    // `type` is in both bases: ambiguous, so it stays dependent.
    using §(ambiguous_base_member)input = typename Combined<X>::type;
};

}  // namespace ambiguous_base_member

namespace diamond_base_member {

template <typename T>
struct Root {
    using type = T;
};

template <typename T>
struct Left : Root<T> {};

template <typename T>
struct Right : Root<T> {};

template <typename T>
struct Joined : Left<T>, Right<T> {};

template <typename X>
struct test {
    using §(diamond_base_member)input = typename Joined<X>::type;
};

}  // namespace diamond_base_member

namespace open_default_shared_base {

template <typename T>
struct Probe {
    static inline bool value = true;
};

template <typename U>
struct Base {
    using type = U*;
};

template <typename T, bool = Probe<T>::value>
struct Choice : Base<T> {};

template <typename T>
struct Choice<T, false> : Base<T> {};

template <typename X>
struct test {
    // The default does not fold, but both patterns reach `type` through `Base<X>`.
    using §(open_default_shared_base)input = typename Choice<X>::type;
};

}  // namespace open_default_shared_base

namespace open_default_different_bases {

template <typename T>
struct Probe {
    static inline bool value = true;
};

template <typename T>
struct Left {};

template <typename T>
struct Right {};

template <typename U>
struct Base {
    using type = U;
};

template <typename T, bool = Probe<T>::value>
struct Choice : Base<Left<T>> {};

template <typename T>
struct Choice<T, false> : Base<Right<T>> {};

template <typename X>
struct test {
    // The patterns reach `type` through different bases: no common answer, it stays dependent.
    using §(open_default_different_bases)input = typename Choice<X>::type;
};

}  // namespace open_default_different_bases
