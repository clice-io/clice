template <typename T>
struct Vec {
    using value_type = T;
    using reference = T&;
    T* data;
    int size() const;
};

template <typename T>
void bar() {
    typename Vec<Vec<T>>::value_type v;
    v.§(pos);
}

template <typename T>
void baz(typename Vec<Vec<T>>::reference r) {
    r.§(reference);
}

template <typename T>
void qux() {
    typename Vec<Vec<T>>::value_type* p;
    p->§(pointee);
}
