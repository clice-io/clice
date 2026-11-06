namespace partial_specialization {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {};

template <typename U2>
struct B {};

template <typename U2, template <typename...> typename HKT>
struct B<HKT<U2>> {
    using type = type_list<U2>;
};

template <typename X>
struct test {
    using §(partial_specialization)input = typename B<A<X>>::type;
};

}  // namespace partial_specialization

namespace conditional_type {

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
    using §(conditional_type)input = typename conditional<true, X, int>::type;
};

}  // namespace conditional_type

namespace dependent_return_type {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
struct remove_reference<T&> {
    using type = T;
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;
};

template <typename X>
struct test {
    using §(dependent_return_type)input = typename remove_reference<X&>::type;
};

}  // namespace dependent_return_type

namespace rvalue_ref_removal {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
struct remove_reference<T&> {
    using type = T;
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;
};

template <typename X>
struct test {
    using §(rvalue_ref_removal)input = typename remove_reference<X&&>::type;
};

}  // namespace rvalue_ref_removal

namespace add_pointer {

template <typename T>
struct add_pointer {
    using type = T*;
};

template <typename T>
struct add_pointer<T&> {
    using type = T*;
};

template <typename X>
struct test {
    using §(add_pointer)input = typename add_pointer<X&>::type;
};

}  // namespace add_pointer

namespace enable_if_like {

template <bool B, typename T = void>
struct enable_if {};

template <typename T>
struct enable_if<true, T> {
    using type = T;
};

template <typename X>
struct test {
    using §(enable_if_like)input = typename enable_if<true, X>::type;
};

}  // namespace enable_if_like

namespace void_specialization {

template <typename T>
struct A {
    using type = T;
};

template <>
struct A<void> {
    using type = int;
};

template <typename X>
struct test {
    using §(void_specialization)input = typename A<X>::type;
};

}  // namespace void_specialization

namespace partial_second_arg {

template <typename... Ts>
struct type_list {};

template <typename A, typename B>
struct combine {
    using type = A;
};

template <typename A, typename B>
struct combine<A, B*> {
    using type = type_list<A, B>;
};

template <typename X, typename Y>
struct test {
    using §(partial_second_arg)input = typename combine<X, Y*>::type;
};

}  // namespace partial_second_arg

namespace template_id_partial {

template <typename... Ts>
struct type_list {};

template <typename T>
struct vec {};

template <typename T>
struct unwrap {
    using type = T;
};

template <typename T>
struct unwrap<vec<T>> {
    using type = type_list<T>;
};

template <typename X>
struct test {
    using §(template_id_partial)input = typename unwrap<vec<X>>::type;
};

}  // namespace template_id_partial

namespace recursive_pointer_peel {

template <typename T>
struct strip {
    using type = T;
};

template <typename T>
struct strip<T*> {
    using type = typename strip<T>::type;
};

template <typename X>
struct test {
    using §(recursive_pointer_peel)input = typename strip<X***>::type;
};

}  // namespace recursive_pointer_peel

namespace conditional_false_type {

template <bool B, typename T, typename F>
struct pick {
    using type = T;
};

template <typename T, typename F>
struct pick<false, T, F> {
    using type = F;
};

template <typename X>
struct test {
    using §(conditional_false_type)input = typename pick<false, int, X>::type;
};

}  // namespace conditional_false_type
