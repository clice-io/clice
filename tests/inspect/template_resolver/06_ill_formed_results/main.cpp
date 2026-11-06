namespace void_reference {

template <typename T, typename X>
struct A {
    using type = T&;
};

template <typename X>
struct test {
    // `void&` does not exist: no reference to `void`.
    using §(void_reference)input = typename A<void, X>::type;
};

}  // namespace void_reference

namespace pointer_to_reference {

template <typename T>
struct P {
    using type = T*;
};

template <typename X>
struct test {
    // A pointer to `X&` does not exist: it stays dependent, no pointer to a reference.
    using §(pointer_to_reference)input = typename P<X&>::type;
};

}  // namespace pointer_to_reference

namespace atomic_reference {

template <typename T, typename U>
struct A {
    using type = _Atomic(T);
};

template <typename X>
struct test {
    // `_Atomic(int&)` does not exist: no atomic reference.
    using §(atomic_reference)input = typename A<int&, X>::type;
};

}  // namespace atomic_reference

namespace negative_array_bound {

template <int N, typename U>
struct A {
    using type = U[N];
};

template <typename X>
struct test {
    // `X[-1]` does not exist: no array of a fixed bound.
    using §(negative_array_bound)input = typename A<-1, X>::type;
};

}  // namespace negative_array_bound

namespace compound_value_degrade {

template <int N>
struct value {};

template <int N>
struct A {
    using type = value<N + 1>;
};

template <int X>
struct test {
    // `N + 1` is not substituted: it stays dependent.
    using §(compound_value_degrade)input = typename A<X>::type;
};

}  // namespace compound_value_degrade

namespace void_function_param {

template <typename T, typename U>
struct A {
    using type = void(T);
};

template <typename X>
struct test {
    // A `void` parameter does not exist: no function taking one.
    using §(void_function_param)input = typename A<void, X>::type;
};

}  // namespace void_function_param

namespace void_member_pointee {

struct C {};

template <typename T, typename U>
struct A {
    using type = T C::*;
};

template <typename X>
struct test {
    // `void C::*` does not exist: no member pointer to `void`.
    using §(void_member_pointee)input = typename A<void, X>::type;
};

}  // namespace void_member_pointee

namespace surplus_arguments {

template <typename T>
struct target {};

template <template <typename...> class TT, typename A>
struct apply {
    using type = TT<A, int>;
};

template <typename X>
struct test {
    // `target` takes one argument: no `target<X, int>`.
    using §(surplus_arguments)input = typename apply<target, X>::type;
};

}  // namespace surplus_arguments

namespace function_return_array {

template <typename T, typename U>
struct A {
    using type = T();
};

template <typename X>
struct test {
    // A function cannot return an array: no such function type.
    using §(function_return_array)input = typename A<int[2], X>::type;
};

}  // namespace function_return_array

namespace dependent_template_head {

template <template <typename> class TT>
struct apply {
    using type = TT<int>;
};

template <typename T>
struct test {
    // The template is itself a dependent name: it stays dependent.
    using §(dependent_template_head)input = typename apply<T::template tmpl>::type;
};

}  // namespace dependent_template_head

namespace self_naming_alias {

template <typename A, typename B>
struct X {
    template <typename U>
    using alias = typename X<B, A>::template alias<U>;
};

template <typename T>
struct test {
    // Each expansion of `alias` names the other order's, without end: no answer.
    using §(self_naming_alias)input = typename X<T, int>::template alias<char>::type;
};

}  // namespace self_naming_alias
