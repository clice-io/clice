namespace pointer_type {

template <typename T>
struct A {
    using type = T*;
};

template <typename X>
struct test {
    using §(pointer_type)input = typename A<X>::type;
};

}  // namespace pointer_type

namespace reference_type {

template <typename T>
struct A {
    using type = T&;
};

template <typename X>
struct test {
    using §(reference_type)input = typename A<X>::type;
};

}  // namespace reference_type

namespace const_qualified {

template <typename T>
struct A {
    using type = const T;
};

template <typename X>
struct test {
    using §(const_qualified)input = typename A<X>::type;
};

}  // namespace const_qualified

namespace dependent_sized_array {

template <typename T>
struct A {
    using type = T;
    using pointer = type*;
};

template <typename X>
struct test {
    using §(dependent_sized_array)input = typename A<X>::pointer;
};

}  // namespace dependent_sized_array

namespace const_pointer_member {

template <typename T>
struct A {
    using type = const T*;
};

template <typename X>
struct test {
    using §(const_pointer_member)input = typename A<X>::type;
};

}  // namespace const_pointer_member

namespace pointer_const_member {

template <typename T>
struct A {
    using type = T* const;
};

template <typename X>
struct test {
    using §(pointer_const_member)input = typename A<X>::type;
};

}  // namespace pointer_const_member

namespace const_ref_typedef {

template <typename T>
struct A {
    using c = const T;
    using type = c&;
};

template <typename X>
struct test {
    using §(const_ref_typedef)input = typename A<X>::type;
};

}  // namespace const_ref_typedef

namespace pointer_chain_ref {

template <typename T>
struct A {
    using p = T*;
    using pp = p*;
    using type = pp&;
};

template <typename X>
struct test {
    using §(pointer_chain_ref)input = typename A<X>::type;
};

}  // namespace pointer_chain_ref

namespace reference_collapse {

template <typename T>
struct R {
    using type = T&;
};

template <typename X>
struct test {
    using §(reference_collapse)input = typename R<X&&>::type;
};

}  // namespace reference_collapse

namespace member_pointer_rewrite {

template <typename T>
struct A {
    using type = T A::*;
};

template <typename X>
struct test {
    using §(member_pointer_rewrite)input = typename A<X>::type;
};

}  // namespace member_pointer_rewrite

namespace atomic_rewrite {

template <typename T>
struct A {
    using type = _Atomic(T);
};

template <typename X>
struct test {
    using §(atomic_rewrite)input = typename A<X>::type;
};

}  // namespace atomic_rewrite

namespace attributed_rewrite {

template <typename T>
struct A {
    using type = T __attribute__((address_space(1)))*;
};

template <typename X>
struct test {
    using §(attributed_rewrite)input = typename A<X>::type;
};

}  // namespace attributed_rewrite

namespace const_reference_drop {

template <typename T>
struct A {
    using type = const T;
};

template <typename X>
struct test {
    // `const` applied to a reference is ignored.
    using §(const_reference_drop)input = typename A<X&>::type;
};

}  // namespace const_reference_drop
