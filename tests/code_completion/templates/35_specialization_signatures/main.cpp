template <class T, int N>
struct Array {
    Array<T, N> copy();
};

template <class... Bs>
struct Pack {
    Pack<Bs...> again();
};

template <int... Ns>
struct Seq {
    Seq<Ns...> again();
};

template <template <class> class... Ts>
struct Kinds {
    Kinds<Ts...> again();
};

template <class... Ts>
struct Views {
    Pack<const Ts...> view();
};

template <class T>
struct X {
    static int f(T value);
};

template <int M, class U, class... Cs>
void use(Array<U, M> a, Pack<Cs...> p) {
    a.§(non_type);
    p.§(pack);
    X<U>::§(qualified);
}

template <int... Ks, template <class> class... Hs>
void more(Seq<Ks...> s, Kinds<Hs...> k) {
    s.§(value_pack);
    k.§(template_pack);
}

template <class... Ws>
void qualified(Views<Ws...> v) {
    v.§(qualified_pack);
}
