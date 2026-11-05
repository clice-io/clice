namespace structured_pack_deduce {

template <typename... Ts>
struct type_list {};

template <typename T>
struct box {};

template <typename T>
struct unbox {};

template <typename... Us>
struct unbox<type_list<box<Us>...>> {
    using type = type_list<Us...>;
};

template <typename X, typename Y>
struct test {
    using §(structured_pack_deduce)input = typename unbox<type_list<box<X>, box<Y>>>::type;
};

}  // namespace structured_pack_deduce

namespace pack_zip_deduce {

template <typename... Ts>
struct type_list {};

template <typename A, typename B>
struct pair {};

template <typename T>
struct split {};

template <typename... As, typename... Bs>
struct split<type_list<pair<As, Bs>...>> {
    using type = type_list<As..., Bs...>;
};

template <typename X, typename Y>
struct test {
    using §(pack_zip_deduce)input = typename split<type_list<pair<X, int>, pair<Y, float>>>::type;
};

}  // namespace pack_zip_deduce

namespace repeated_pack_element {

template <typename A, typename B>
struct pair {};

template <typename... Ts>
struct type_list {};

template <typename T>
struct trait {
    using type = void;
};

template <typename... Ts>
struct trait<type_list<pair<Ts, Ts>...>> {
    using type = int;
};

template <typename X, typename Y>
struct test {
    // `pair<X, Y>` does not repeat its type: the primary's `void`.
    using §(repeated_pack_element)input = typename trait<type_list<pair<X, Y>, pair<Y, Y>>>::type;
};

}  // namespace repeated_pack_element

namespace pack_prefix_arity {

template <typename T>
struct unary {};

template <template <typename...> class G>
struct token {};

template <typename T, typename U>
struct P {
    using type = void;
};

template <template <typename, typename, typename...> class TT, typename U>
struct P<token<TT>, U> {
    using type = int;
};

template <typename X>
struct test {
    // `unary` cannot supply the two leading parameters of `TT`: the primary's `void`.
    using §(pack_prefix_arity)input = typename P<token<unary>, X>::type;
};

}  // namespace pack_prefix_arity

namespace mixed_pack_candidate {

template <typename T, int N = 0>
struct mixed {};

template <typename T>
struct trait {
    using type = void;
};

template <template <typename...> class TT, typename... Us>
struct trait<TT<Us...>> {
    using type = int;
};

template <typename X>
struct test {
    // A type pack cannot cover the value parameter of `mixed`: the primary's `void`.
    using §(mixed_pack_candidate)input = typename trait<mixed<X>>::type;
};

}  // namespace mixed_pack_candidate

namespace empty_pack_conflict {

template <typename... Ts>
struct list {};

template <typename... Ts>
struct tuple {};

template <typename A, typename B>
struct P {
    using type = void;
};

template <typename... Ts>
struct P<list<Ts...>, tuple<Ts...>> {
    using type = int;
};

template <typename X>
struct test {
    // `Ts` is `X` from `list` but empty from `tuple`: the primary's `void`.
    using §(empty_pack_conflict)input = typename P<list<X>, tuple<>>::type;
};

}  // namespace empty_pack_conflict

namespace non_trailing_suffix {

template <typename... Ts>
struct type_list {};

template <typename A, typename B>
struct trait {
    using type = void;
};

template <typename... Ts>
struct trait<type_list<Ts...>, void(Ts..., int)> {
    using type = int;
};

template <typename X>
struct test {
    // `Ts` is `X` once the trailing `int` is set aside: the partial's `int`.
    using §(non_trailing_suffix)input = typename trait<type_list<X>, void(X, int)>::type;
};

}  // namespace non_trailing_suffix

namespace argument_pack_mismatch {

template <typename A, typename B>
struct trait {
    using type = void;
};

template <typename T>
struct trait<T, void(int)> {
    using type = int;
};

template <typename... Us>
struct test {
    // `void(Us*...)` takes only pointers, never `void(int)`: the primary's `void`.
    using §(argument_pack_mismatch)input = typename trait<int, void(Us*...)>::type;
};

}  // namespace argument_pack_mismatch

namespace template_arity_mismatch {

template <typename A, typename B>
struct binary {};

template <typename T>
struct trait {
    using type = void;
};

template <template <typename> class TT, typename... Us>
struct trait<TT<Us...>> {
    using type = int;
};

template <typename X>
struct test {
    // `binary` takes two arguments, `TT` one: the primary's `void`.
    using §(template_arity_mismatch)input = typename trait<binary<X, int>>::type;
};

}  // namespace template_arity_mismatch

namespace repeated_value_pack {

template <int A, int B>
struct pairv {};

template <typename... Ts>
struct type_list {};

template <typename T>
struct trait {
    using type = void;
};

template <int... Ns>
struct trait<type_list<pairv<Ns, Ns>...>> {
    using type = int;
};

template <int X>
struct test {
    using §(repeated_value_pack)input = typename trait<type_list<pairv<X, X>>>::type;
};

}  // namespace repeated_value_pack

namespace non_trailing_pack {

template <typename... Ts>
struct type_list {};

template <typename A, typename B>
struct trait {
    using type = void;
};

template <typename... Ts>
struct trait<type_list<Ts...>, void(Ts..., int)> {
    using type = int;
};

template <typename X>
struct test {
    // `void(X)` lacks the trailing `int`: the primary's `void`.
    using §(non_trailing_pack)input = typename trait<type_list<X>, void(X)>::type;
};

}  // namespace non_trailing_pack
