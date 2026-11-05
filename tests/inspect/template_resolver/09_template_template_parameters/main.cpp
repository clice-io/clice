namespace template_template_replace {

template <typename T>
struct box {};

template <typename A, typename U>
struct replace_first {};

template <template <typename, typename...> typename TT,
          typename U,
          typename T,
          typename... Ts>
struct replace_first<TT<T, Ts...>, U> {
    using type = TT<U, Ts...>;
};

template <typename X>
struct test {
    using §(template_template_replace)input = typename replace_first<box<X>, int>::type;
};

}  // namespace template_template_replace

namespace rebind_trailing_args {

template <typename A, typename B, typename C>
struct triple {};

template <typename Old, typename New>
struct replace_first {};

template <template <typename...> typename TT,
          typename New,
          typename T,
          typename... Rest>
struct replace_first<TT<T, Rest...>, New> {
    using type = TT<New, Rest...>;
};

template <typename X, typename Y>
struct test {
    using §(rebind_trailing_args)input = typename replace_first<triple<int, X, Y>, float>::type;
};

}  // namespace rebind_trailing_args

namespace template_pack_extract {

template <typename... Ts>
struct type_list {};

template <typename T>
struct wrap {};

template <template <typename...> typename TT, typename... Us>
struct wrap<TT<Us...>> {
    using type = TT<Us...>;
};

template <typename X, typename Y>
struct test {
    using §(template_pack_extract)input = typename wrap<type_list<X, Y>>::type;
};

}  // namespace template_pack_extract

namespace template_swap_args {

template <typename A, typename B>
struct pair {};

template <typename T>
struct first_of {};

template <template <typename, typename> typename TT, typename A, typename B>
struct first_of<TT<A, B>> {
    using type = TT<B, A>;
};

template <typename X, typename Y>
struct test {
    using §(template_swap_args)input = typename first_of<pair<X, Y>>::type;
};

}  // namespace template_swap_args

namespace template_pack_deduce {

template <typename T>
struct box {};

template <template <typename> class... TTs>
struct A {
    using type = int;
};

template <template <typename> class TX>
struct test {
    using §(template_pack_deduce)input = typename A<TX, box>::type;
};

}  // namespace template_pack_deduce

namespace alias_template_head {

template <typename T, typename U>
struct pair {};

template <typename T, typename U>
using alias_pair = pair<T, U>;

template <template <typename, typename> class TT, typename A, typename B>
struct apply {
    using type = TT<A, B>;
};

template <typename X>
struct test {
    using §(alias_template_head)input = typename apply<alias_pair, X, int>::type;
};

}  // namespace alias_template_head

namespace dependent_name_argument {

template <template <typename> class TT>
struct apply {};

template <typename T>
struct A {
    using type = apply<T::template tmpl>;
};

template <typename X>
struct test {
    using §(dependent_name_argument)input = typename A<X>::type;
};

}  // namespace dependent_name_argument

namespace template_through_layer {

template <typename... Ts>
struct type_list {};

template <template <typename...> typename TT, typename... Us>
struct apply {
    using type = TT<Us...>;
};

template <template <typename...> typename TT, typename... Us>
struct indirect {
    using type = typename apply<TT, Us...>::type;
};

template <typename X, typename Y>
struct test {
    using §(template_through_layer)input = typename indirect<type_list, X, Y>::type;
};

}  // namespace template_through_layer
