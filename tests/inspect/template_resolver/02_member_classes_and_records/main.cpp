namespace outer_dependent_member_class {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    template <typename T2>
    struct B {
        template <typename T3>
        struct C {
            using type = type_list<T1, T2, T3>;
        };
    };
};

template <typename X, typename Y, typename Z>
struct test {
    using §(outer_dependent_member_class)input = typename A<X>::template B<Y>::template C<Z>::type;
};

}  // namespace outer_dependent_member_class

namespace inner_dependent_member_class {

template <typename... Ts>
struct type_list {};

template <typename T>
struct test {
    template <int N, typename U>
    struct B {
        using type = type_list<U, T>;
    };

    using §(inner_dependent_member_class)input = typename B<1, T>::type;
};

}  // namespace inner_dependent_member_class

namespace inner_partial_member {

template <typename... Ts>
struct type_list {};

template <typename T, typename U>
struct test {};

template <typename T>
struct test<T, T> {
    template <int N, typename U>
    struct A {
        using type = type_list<U, T>;
    };

    using §(inner_partial_member)input = typename A<1, T>::type;
};

}  // namespace inner_partial_member

namespace nested_lookup {

template <typename... Ts>
struct type_list {};

template <typename T>
struct A {
    template <typename U>
    struct B {
        using type = type_list<T, U>;
    };
};

template <typename X, typename Y>
struct test {
    using §(nested_lookup)input = typename A<X>::template B<Y>::type;
};

}  // namespace nested_lookup

namespace nested_member_depth_mix {

template <typename... Ts>
struct type_list {};

template <typename T>
struct Outer {
    using outer_type = T*;

    template <typename U>
    struct Inner {
        using type = type_list<outer_type, U>;
    };
};

template <typename X, typename Y>
struct test {
    using §(nested_member_depth_mix)input = typename Outer<X>::template Inner<Y>::type;
};

}  // namespace nested_member_depth_mix

namespace outer_param_mismatch {

template <typename A, typename B>
struct pair {};

template <typename O>
struct Outer {
    template <typename T>
    struct Inner {
        using type = void;
    };

    template <typename U>
    struct Inner<pair<O, U>> {
        using type = U;
    };
};

template <typename X, typename Y>
struct test {
    // `pair<Y, int>` does not start with Outer's `X`: the primary's `void`.
    using §(outer_param_mismatch)input = typename Outer<X>::template Inner<pair<Y, int>>::type;
};

}  // namespace outer_param_mismatch

namespace outer_param_match {

template <typename A, typename B>
struct pair {};

template <typename O>
struct Outer {
    template <typename T>
    struct Inner {
        using type = void;
    };

    template <typename U>
    struct Inner<pair<O, U>> {
        using type = U;
    };
};

template <typename X>
struct test {
    using §(outer_param_match)input = typename Outer<X>::template Inner<pair<X, int>>::type;
};

}  // namespace outer_param_match

namespace template_default_forward {

template <typename T>
struct box {};

template <template <typename> class TT, template <typename> class U = TT>
struct A {
    template <typename X>
    using type = U<X>;
};

template <typename X>
struct test {
    using §(template_default_forward)input = typename A<box>::template type<X>;
};

}  // namespace template_default_forward

namespace concrete_scope_member_template {

template <typename A>
struct traits {
    template <typename U>
    struct rebind {
        using other = U;
    };
};

template <typename T, typename A>
struct box {
    using type = typename A::template rebind<T>::other;
};

template <typename X>
struct test {
    using §(concrete_scope_member_template)input = typename box<X, traits<int>>::type;
};

}  // namespace concrete_scope_member_template

namespace concrete_scope_template_argument {

template <typename A>
struct traits {
    template <typename U>
    struct rebind {
        using other = U;
    };
};

template <template <typename> class TT, typename U>
struct apply {
    using type = typename TT<U>::other;
};

template <typename T, typename U>
struct box {
    using type = typename apply<T::template rebind, U>::type;
};

template <typename X>
struct test {
    using §(concrete_scope_template_argument)input = typename box<traits<int>, X>::type;
};

}  // namespace concrete_scope_template_argument

namespace concrete_outer_member_template {

template <typename T>
struct Outer {
    template <typename U>
    struct Inner {
        using type = U*;
    };
};

template <typename X>
struct test {
    // `Outer<int>::Inner` waits for an instantiation to have members: they are
    // `Outer<T>::Inner`'s.
    using §(concrete_outer_member_template)input = typename Outer<int>::template Inner<X>::type;
    using §(concrete_outer_member_record)inner = typename Outer<int>::template Inner<X>;
};

}  // namespace concrete_outer_member_template

namespace nested_class_member {

template <typename T>
struct Host {
    struct Nested {
        using value = T*;
    };
};

template <typename X>
struct test {
    using §(nested_class_member)input = typename Host<X>::Nested::value;
};

}  // namespace nested_class_member

namespace plain_record_member {

struct Plain {
    using value = int;
};

template <typename T>
struct Holder {
    using type = Plain;
};

template <typename X>
struct test {
    using §(plain_record_member)input = typename Holder<X>::type::value;
};

}  // namespace plain_record_member

namespace explicit_specialization_member {

template <typename T>
struct Box {
    using inner = void;
};

template <>
struct Box<int> {
    using inner = char;
};

template <typename T>
struct Holder {
    using type = Box<int>;
};

template <typename X>
struct test {
    using §(explicit_specialization_member)input = typename Holder<X>::type::inner;
};

}  // namespace explicit_specialization_member

namespace record_of_member_alias {

template <typename T>
struct Vec {
    using value_type = T;
};

template <typename X>
struct test {
    // The class is `Vec`, not the parameter `T` its alias names.
    using §(record_of_member_alias)input = typename Vec<Vec<X>>::value_type;
};

}  // namespace record_of_member_alias

namespace record_of_partial_pattern {

template <typename T>
struct Traits {};

template <typename T>
struct Traits<T*> {};

template <typename X>
struct test {
    // The class is the partial specialization instantiation would use.
    using §(record_of_partial_pattern)input = Traits<X*>;
};

}  // namespace record_of_partial_pattern

namespace record_of_parameter {

template <typename X>
struct test {
    // A bare parameter names no class.
    using §(record_of_parameter)input = X;
};

}  // namespace record_of_parameter
