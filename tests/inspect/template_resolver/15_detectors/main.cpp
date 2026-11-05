namespace sfinae_rebind_present {

template <typename... Ts>
using void_t = void;

template <typename T>
struct alloc {
    template <typename U>
    struct rebind {
        using other = alloc<U>;
    };
};

template <typename A, typename U, typename = void>
struct rebind_helper {
    using type = int;
};

template <typename A, typename U>
struct rebind_helper<A, U, void_t<typename A::template rebind<U>::other>> {
    using type = typename A::template rebind<U>::other;
};

template <typename X>
struct test {
    using §(sfinae_rebind_present)input = typename rebind_helper<alloc<X>, float>::type;
};

}  // namespace sfinae_rebind_present

namespace sfinae_rebind_absent {

template <typename... Ts>
using void_t = void;

template <typename T>
struct plain {};

template <typename A, typename U>
struct replace_first {};

template <template <typename, typename...> typename TT,
          typename U,
          typename T,
          typename... Ts>
struct replace_first<TT<T, Ts...>, U> {
    using type = TT<U, Ts...>;
};

template <typename A, typename U, typename = void>
struct rebind_helper {
    using type = typename replace_first<A, U>::type;
};

template <typename A, typename U>
struct rebind_helper<A, U, void_t<typename A::template rebind<U>::other>> {
    using type = typename A::template rebind<U>::other;
};

template <typename X>
struct test {
    using §(sfinae_rebind_absent)input = typename rebind_helper<plain<X>, int>::type;
};

}  // namespace sfinae_rebind_absent

namespace detector_member_present {

template <typename... Ts>
using void_t = void;

template <typename... Ts>
struct type_list {};

template <typename T, typename = void>
struct detect {
    using type = int;
};

template <typename T>
struct detect<T, void_t<typename T::element>> {
    using type = typename T::element;
};

template <typename T>
struct has_elem {
    using element = type_list<T>;
};

template <typename X>
struct test {
    using §(detector_member_present)input = typename detect<has_elem<X>>::type;
};

}  // namespace detector_member_present

namespace detector_member_absent {

template <typename... Ts>
using void_t = void;

template <typename... Ts>
struct type_list {};

template <typename T, typename = void>
struct detect {
    using type = type_list<T>;
};

template <typename T>
struct detect<T, void_t<typename T::element>> {
    using type = typename T::element;
};

template <typename T>
struct no_elem {
    using other = int;
};

template <typename X>
struct test {
    using §(detector_member_absent)input = typename detect<no_elem<X>>::type;
};

}  // namespace detector_member_absent

namespace detector_via_base {

template <typename... Ts>
using void_t = void;

template <typename... Ts>
struct type_list {};

template <typename T, typename = void>
struct detect {
    using type = int;
};

template <typename T>
struct detect<T, void_t<typename T::element>> {
    using type = typename T::element;
};

template <typename T>
struct elem_base {
    using element = type_list<T>;
};

template <typename T>
struct elem_derived : elem_base<T> {};

template <typename X>
struct test {
    using §(detector_via_base)input = typename detect<elem_derived<X>>::type;
};

}  // namespace detector_via_base

namespace nested_rebind_detector {

template <typename... Ts>
using void_t = void;

template <typename T>
struct alloc {
    template <typename U>
    struct rebind {
        using other = alloc<U>;
    };
};

template <typename A, typename U, typename = void>
struct rebind_inner {
    using type = int;
};

template <typename A, typename U>
struct rebind_inner<A, U, void_t<typename A::template rebind<U>::other>> {
    using type = typename A::template rebind<U>::other;
};

template <typename A, typename U>
struct rebind_outer {
    using type = typename rebind_inner<A, U>::type;
};

template <typename X>
struct test {
    using §(nested_rebind_detector)input = typename rebind_outer<alloc<X>, float>::type;
};

}  // namespace nested_rebind_detector

namespace recursive_detector_probe {

template <typename... Ts>
using void_t = void;

template <typename... Ts>
struct type_list {};

template <typename T, typename = void>
struct wrap {
    using type = type_list<T>;
};

template <typename T>
struct wrap<T*, void_t<typename wrap<T>::type>> {
    using type = typename wrap<T>::type;
};

template <typename X>
struct test {
    using §(recursive_detector_probe)input = typename wrap<X*>::type;
};

}  // namespace recursive_detector_probe

namespace dependent_scope_inconclusive {

template <typename... Ts>
using void_t = void;

template <typename T>
struct A {};

template <typename T>
struct A<T*> {
    using foo = int;
};

template <typename T, typename = void>
struct detect {
    using type = void;
};

template <typename T>
struct detect<T, void_t<typename A<T>::foo>> {
    using type = int;
};

template <typename X>
struct test {
    // `A<X>::foo` may exist, as `A<T*>` declares it: the detector partial's `int`.
    using §(dependent_scope_inconclusive)input = typename detect<X>::type;
};

}  // namespace dependent_scope_inconclusive

namespace template_member_probe {

template <typename... Ts>
using void_t = void;

template <typename T, typename = void>
struct detect {
    using type = void;
};

template <typename T>
struct detect<T, void_t<typename T::foo>> {
    using type = int;
};

template <typename T>
struct wrap {
    template <typename U>
    struct foo {};
};

template <typename X>
struct test {
    // `wrap<X>::foo` is a class template, not a type: the primary's `void`.
    using §(template_member_probe)input = typename detect<wrap<X>>::type;
};

}  // namespace template_member_probe

namespace dependent_base_probe {

template <typename... Ts>
using void_t = void;

template <typename T>
struct D : T {};

template <typename T, typename = void>
struct detect {
    using type = void;
};

template <typename T>
struct detect<T, void_t<typename D<T>::foo>> {
    using type = int;
};

template <typename X>
struct test {
    // `D<X>` may inherit `foo` from `X`: the detector partial's `int`.
    using §(dependent_base_probe)input = typename detect<X>::type;
};

}  // namespace dependent_base_probe

namespace private_member_probe {

template <typename... Ts>
using void_t = void;

template <typename T, typename = void>
struct detect {
    using type = void;
};

template <typename T>
struct detect<T, void_t<typename T::secret>> {
    using type = int;
};

template <typename T>
class wrap {
    using secret = int;
};

template <typename X>
struct test {
    // `secret` is private: the primary's `void`.
    using §(private_member_probe)input = typename detect<wrap<X>>::type;
};

}  // namespace private_member_probe

namespace value_member_probe {

template <typename... Ts>
using void_t = void;

template <typename T, typename = void>
struct detect {
    using type = void;
};

template <typename T>
struct detect<T, void_t<typename T::value>> {
    using type = int;
};

template <typename T>
struct wrap {
    static constexpr int value = 1;
};

template <typename X>
struct test {
    // `value` is a static data member, not a type: the primary's `void`.
    using §(value_member_probe)input = typename detect<wrap<X>>::type;
};

}  // namespace value_member_probe
