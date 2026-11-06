namespace type_parameter_type {

template <typename T>
struct A {
    using type = T;
};

template <typename X>
struct test {
    using §(type_parameter_type)input = typename A<X>::type;
};

}  // namespace type_parameter_type

namespace single_level {

template <typename... Ts>
struct type_list {};

template <typename T>
struct A {
    using type = type_list<T>;
};

template <typename X>
struct test {
    using §(single_level)input = typename A<X>::type;
};

}  // namespace single_level

namespace single_level_not_dependent {

template <typename T>
struct A {
    using type = int;
};

template <typename X>
struct test {
    using §(single_level_not_dependent)input = typename A<X>::type;
};

}  // namespace single_level_not_dependent

namespace multi_level {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = type_list<T1>;
};

template <typename T2>
struct B {
    using type = typename A<T2>::type;
};

template <typename T3>
struct C {
    using type = typename B<T3>::type;
};

template <typename X>
struct test {
    using §(multi_level)input = typename C<X>::type;
};

}  // namespace multi_level

namespace multi_level_not_dependent {

template <typename T1>
struct A {
    using type = int;
};

template <typename T2>
struct B {
    using type = typename A<T2>::type;
};

template <typename T3>
struct C {
    using type = typename B<T3>::type;
};

template <typename X>
struct test {
    using §(multi_level_not_dependent)input = typename C<X>::type;
};

}  // namespace multi_level_not_dependent

namespace argument_dependent {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = T1;
};

template <typename T2>
struct B {
    using type = type_list<T2>;
};

template <typename X>
struct test {
    using §(argument_dependent)input = typename B<typename A<X>::type>::type;
};

}  // namespace argument_dependent

namespace alias_argument {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = T1;
};

template <typename T2>
struct B {
    using base = A<T2>;
    using type = type_list<typename base::type>;
};

template <typename X>
struct test {
    using §(alias_argument)input = typename B<X>::type;
};

}  // namespace alias_argument

namespace alias_dependent {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = type_list<T1>;
};

template <typename T2>
struct B {
    using base = A<T2>;
    using type = typename base::type;
};

template <typename X>
struct test {
    using §(alias_dependent)input = typename B<X>::type;
};

}  // namespace alias_dependent

namespace alias_template {

template <typename... Ts>
struct type_list {};

template <typename T1, typename U1>
struct A {
    using type = type_list<T1, U1>;
};

template <typename T2>
struct B {
    template <typename U2>
    using type = typename A<T2, U2>::type;
};

template <typename X, typename Y>
struct test {
    using §(alias_template)input = typename B<X>::template type<Y>;
};

}  // namespace alias_template

namespace multi_nested {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using self = A<T1>;
    using type = type_list<T1>;
};

template <typename X>
struct test {
    using §(multi_nested)input = typename A<X>::self::self::self::self::self::type;
};

}  // namespace multi_nested

namespace typedef_chain {

template <typename T>
struct A {
    using step1 = T;
    using step2 = step1;
    using step3 = step2;
    using type = step3;
};

template <typename X>
struct test {
    using §(typedef_chain)input = typename A<X>::type;
};

}  // namespace typedef_chain

namespace dependent_base_typedef {

template <typename... Ts>
struct type_list {};

template <typename T>
struct Base {
    using value_type = T;
};

template <typename T>
struct Derived {
    using base = Base<T>;
    using type = typename base::value_type;
};

template <typename X>
struct test {
    using §(dependent_base_typedef)input = typename Derived<X>::type;
};

}  // namespace dependent_base_typedef

namespace identity_alias {

template <typename T>
using identity = T;

template <typename T>
struct A {
    using type = identity<T>;
};

template <typename X>
struct test {
    using §(identity_alias)input = typename A<X>::type;
};

}  // namespace identity_alias

namespace self_referential_alias {

template <typename T>
struct Wrapper {
    using self = Wrapper<T>;
    using type = T;
};

template <typename X>
struct test {
    using §(self_referential_alias)input = typename Wrapper<X>::self::self::type;
};

}  // namespace self_referential_alias

namespace typedef_chain_four_templates {

template <typename... Ts>
struct type_list {};

template <typename T1>
struct A {
    using type = type_list<T1>;
};

template <typename T2>
struct B {
    using type = typename A<T2>::type;
};

template <typename T3>
struct C {
    using type = typename B<T3>::type;
};

template <typename T4>
struct D {
    using type = typename C<T4>::type;
};

template <typename X>
struct test {
    using §(typedef_chain_four_templates)input = typename D<X>::type;
};

}  // namespace typedef_chain_four_templates

namespace injected_class_name {

template <typename T>
struct A {
    using type = T;
    using self = A;
};

template <typename X>
struct test {
    using §(injected_class_name)input = typename A<X>::self::type;
};

}  // namespace injected_class_name

namespace alias_template_chain {

template <typename T>
using first = T;

template <typename T>
using second = first<T>;

template <typename T>
struct A {
    using type = second<T>;
};

template <typename X>
struct test {
    using §(alias_template_chain)input = typename A<X>::type;
};

}  // namespace alias_template_chain

namespace class_template_alias_target {

template <typename T>
struct Impl {
    using type = T;
};

template <typename T>
using Alias = Impl<T>;

template <typename X>
struct test {
    using §(class_template_alias_target)input = typename Alias<X>::type;
};

}  // namespace class_template_alias_target

namespace redecl_split_definition {

template <typename T>
struct A;

template <typename X>
struct test {
    using §(redecl_split_definition)input = typename A<X>::type;
};

template <typename T>
struct A {
    using type = T*;
};

}  // namespace redecl_split_definition

namespace redecl_alias_forward {

template <typename T>
struct A;

template <typename T>
using ref = A<T>;

template <typename X>
struct test {
    using §(redecl_alias_forward)input = typename ref<X>::type;
};

template <typename T>
struct A {
    using type = T*;
};

}  // namespace redecl_alias_forward

namespace plain_variable {

template <typename T>
struct holder {
    using type = T*;
};

template <typename T>
void use() {
    // A variable without `auto` shows its declared type, resolved.
    typename holder<T>::type §(plain_variable)value = nullptr;
}

}  // namespace plain_variable
