template <class F>
struct traits : traits<decltype(&F::operator())> {};

template <class F>
void cyclic() {
    typename traits<F>::result r;
}

template <class T>
struct left {
    using value_type = int;
};

template <class T>
struct right {
    using value_type = long;
};

template <class T>
struct both : left<T>, right<T> {};

template <class T>
void ambiguous() {
    typename both<T>::value_type v;
}

template <bool B>
struct pick {
    using type = int;
};

template <>
struct pick<false> {
    using type = long;
};

template <int N>
void undecided() {
    typename pick<N == 0>::type p;
}
