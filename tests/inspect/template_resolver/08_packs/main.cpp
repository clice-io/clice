namespace multi_element_pack {

template <typename... Ts>
struct type_list {};

template <typename... Us>
struct A {
    using type = type_list<int, Us...>;
};

template <typename X, typename Y>
struct test {
    using §(multi_element_pack)input = typename A<X, Y>::type;
};

}  // namespace multi_element_pack

namespace pack_expansion {

template <typename... Ts>
struct type_list {};

template <typename U, typename... Us>
struct X {
    using type = type_list<Us...>;
};

template <typename... Ts>
struct test {
    using §(pack_expansion)input = typename X<int, Ts...>::type;
};

}  // namespace pack_expansion

namespace multiple_packs {

template <typename... Ts>
struct type_list {};

template <typename T, typename... Us>
struct A {
    using type = type_list<T, Us...>;
};

template <typename X, typename... Ys>
struct test {
    using §(multiple_packs)input = typename A<X, Ys...>::type;
};

}  // namespace multiple_packs

namespace pack_leading_fixed {

template <typename... Ts>
struct type_list {};

template <typename A, typename B, typename... Rest>
struct pick {
    using type = type_list<Rest..., A, B>;
};

template <typename X, typename Y, typename Z, typename W>
struct test {
    using §(pack_leading_fixed)input = typename pick<X, Y, Z, W>::type;
};

}  // namespace pack_leading_fixed

namespace empty_pack_deduced {

template <typename... Ts>
struct type_list {};

template <typename A, typename... Rest>
struct pick {
    using type = type_list<A, Rest...>;
};

template <typename X>
struct test {
    using §(empty_pack_deduced)input = typename pick<X>::type;
};

}  // namespace empty_pack_deduced

namespace pack_through_two_layers {

template <typename... Ts>
struct type_list {};

template <typename... Us>
struct Inner {
    using type = type_list<Us...>;
};

template <typename... Vs>
struct Outer {
    using type = typename Inner<Vs...>::type;
};

template <typename... Ts>
struct test {
    using §(pack_through_two_layers)input = typename Outer<Ts...>::type;
};

}  // namespace pack_through_two_layers

namespace mixed_pack_elements {

template <typename... Ts>
struct type_list {};

template <typename... Us>
struct A {
    using type = type_list<Us...>;
};

template <typename X>
struct test {
    using §(mixed_pack_elements)input = typename A<int, X, float>::type;
};

}  // namespace mixed_pack_elements

namespace pack_element_cache {

template <typename T>
struct box {
    using type = T;
};

template <typename... Ts>
struct type_list {};

template <typename... Ts>
struct A {
    using type = type_list<typename box<Ts>::type...>;
};

template <typename X>
struct test {
    // Each element resolves `box<Ts>::type` under its own binding.
    using §(pack_element_cache)input = typename A<X, int>::type;
};

}  // namespace pack_element_cache

namespace value_pack_splice {

template <int... Ns>
struct values {};

template <int... Ns>
struct A {
    using type = values<Ns...>;
};

template <int X>
struct test {
    using §(value_pack_splice)input = typename A<1, X>::type;
};

}  // namespace value_pack_splice

namespace template_pack_expansion {

template <typename T>
struct box {};

template <typename... Ts>
struct type_list {};

template <template <typename> class... Fs>
struct A {
    using type = type_list<Fs<int>...>;
};

template <template <typename> class TX>
struct test {
    using §(template_pack_expansion)input = typename A<TX, box>::type;
};

}  // namespace template_pack_expansion

namespace pack_pattern_expansion {

template <typename T>
struct box {};

template <typename... Ts>
struct type_list {};

template <typename... Us>
struct A {
    using type = type_list<box<Us>...>;
};

template <typename X>
struct test {
    using §(pack_pattern_expansion)input = typename A<X, int>::type;
};

}  // namespace pack_pattern_expansion

namespace alias_pack_expanded {

template <typename... Ts>
struct type_list {};

template <typename... Ts>
using listify = type_list<Ts...>;

template <typename... Us>
struct A {
    using type = listify<Us...>;
};

template <typename X>
struct test {
    using §(alias_pack_expanded)input = typename A<X, int>::type;
};

}  // namespace alias_pack_expanded

namespace template_expansion_splice {

template <typename T>
struct box {};

template <template <typename> class... Gs>
struct tlist {};

template <template <typename> class... Fs>
struct A {
    using type = tlist<Fs...>;
};

template <template <typename> class TX>
struct test {
    using §(template_expansion_splice)input = typename A<TX, box>::type;
};

}  // namespace template_expansion_splice

namespace value_pack_expansion {

template <int N>
struct value {};

template <typename... Ts>
struct type_list {};

template <int... Ns>
struct A {
    using type = type_list<value<Ns>...>;
};

template <int X>
struct test {
    using §(value_pack_expansion)input = typename A<X, 2>::type;
};

}  // namespace value_pack_expansion
