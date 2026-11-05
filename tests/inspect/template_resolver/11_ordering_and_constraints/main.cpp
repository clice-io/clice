namespace pointer_cv_ordering {

template <typename... Ts>
struct type_list {};

template <typename T>
struct trait {
    using type = type_list<T>;
};

template <typename T>
struct trait<T*> {
    using type = type_list<T, T>;
};

template <typename T>
struct trait<const T*> {
    using type = type_list<T, T, T>;
};

template <typename X>
struct test {
    using §(pointer_cv_ordering)input = typename trait<const X*>::type;
};

}  // namespace pointer_cv_ordering

namespace pointer_double_ordering {

template <typename... Ts>
struct type_list {};

template <typename T>
struct rank {
    using type = type_list<T>;
};

template <typename T>
struct rank<T*> {
    using type = type_list<T, T>;
};

template <typename T>
struct rank<T**> {
    using type = type_list<T, T, T>;
};

template <typename X>
struct test {
    using §(pointer_double_ordering)input = typename rank<X**>::type;
};

}  // namespace pointer_double_ordering

namespace ref_partial_ordering {

template <typename... Ts>
struct type_list {};

template <typename T>
struct classify {
    using type = type_list<T>;
};

template <typename T>
struct classify<T*> {
    using type = type_list<T, T>;
};

template <typename T>
struct classify<T&> {
    using type = type_list<T, T, T>;
};

template <typename X>
struct test {
    using §(ref_partial_ordering)input = typename classify<X*>::type;
};

}  // namespace ref_partial_ordering

namespace ambiguous_partials {

template <typename A, typename B>
struct trait {
    using type = void;
};

template <typename T, typename U>
struct trait<T*, U> {
    using type = int;
};

template <typename T, typename U>
struct trait<T, U*> {
    using type = char;
};

template <typename X, typename Y>
struct test {
    // Both partials match and neither is more specialized: it stays dependent.
    using §(ambiguous_partials)input = typename trait<X*, Y*>::type;
};

}  // namespace ambiguous_partials

namespace constrained_partial {

template <typename T>
struct P {
    using type = void;
};

template <typename T>
    requires false
struct P<T*> {
    using type = int;
};

template <typename X>
struct test {
    using §(constrained_partial)input = typename P<X*>::type;
};

}  // namespace constrained_partial

namespace constrained_true_partial {

template <typename T>
struct P {
    using type = void;
};

template <typename T>
    requires true
struct P<T*> {
    using type = int;
};

template <typename X>
struct test {
    using §(constrained_true_partial)input = typename P<X*>::type;
};

}  // namespace constrained_true_partial

namespace constrained_value_partial {

template <int N>
struct P {
    using type = void;
};

template <int N>
    requires (N > 0)
struct P<N> {
    using type = int;
};

template <typename T, int N>
struct apply {
    using type = typename P<N>::type;
};

template <typename X>
struct test {
    // `-1 > 0` is false: the primary's `void`.
    using §(constrained_value_partial)input = typename apply<X, -1>::type;
};

}  // namespace constrained_value_partial

namespace constrained_unknown_degrades {

template <typename T>
concept small = sizeof(T) < 4;

template <typename T>
struct P {
    using type = void;
};

template <small T>
struct P<T*> {
    using type = int;
};

template <typename X>
struct test {
    // `small<X>` cannot be decided: it stays dependent.
    using §(constrained_unknown_degrades)input = typename P<X*>::type;
};

}  // namespace constrained_unknown_degrades

namespace constrained_beats_unconstrained {

template <typename T>
struct P {
    using type = void;
};

template <typename T>
struct P<T*> {
    using type = char;
};

template <typename T>
    requires true
struct P<T*> {
    using type = int;
};

template <typename X>
struct test {
    // The constrained partial is the more specialized one.
    using §(constrained_beats_unconstrained)input = typename P<X*>::type;
};

}  // namespace constrained_beats_unconstrained

namespace constraint_disjunction {

template <typename T>
concept small = sizeof(T) < 4;

template <typename T, int N>
struct P {
    using type = void;
};

template <typename T, int N>
    requires (N > 0) || small<T>
struct P<T*, N> {
    using type = int;
};

template <typename T, int N>
struct apply {
    using type = typename P<T*, N>::type;
};

template <typename X>
struct test {
    // `N > 0` holds: the disjunction is satisfied whatever `small<X>` is.
    using §(constraint_disjunction)input = typename apply<X, 1>::type;
};

}  // namespace constraint_disjunction

namespace atomic_constraint_unverified {

template <typename T, typename U>
struct P {
    using type = char;
};

template <typename T, typename U>
    requires (bool(sizeof(typename U::missing) || true))
struct P<T*, U> {
    using type = int;
};

template <typename X>
struct test {
    // `typename U::missing` may fail inside it: `|| true` proves nothing, it stays dependent.
    using §(atomic_constraint_unverified)input = typename P<X*, int>::type;
};

}  // namespace atomic_constraint_unverified
