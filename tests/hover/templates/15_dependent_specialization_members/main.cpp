template <class T, int N>
struct Array {
    Array<T, N> copy();
    T first();
};

template <class... Bs>
struct Pack {
    Pack<Bs...> again();
};

template <class T>
struct X {
    static int f(T value);
};

template <int M, class U, class... Cs>
void use(Array<U, M> a, Pack<Cs...> p) {
    a.§(non_type)copy();
    a.§(type)first();
    p.§(pack)again();
    X<U>::§(qualified)f;
}
