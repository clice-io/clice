template <int N>
struct Arr {
    int primary;
};

template <>
struct Arr<3> {
    int three;
};

template <class T>
struct Box {
    int boxed;
};

template <class T>
struct Wrap {
    int primary;
};

template <>
struct Wrap<int> {
    int int_wrapped;
};

template <class T>
struct Ptr {
    int primary;
};

template <class T>
struct Ptr<T*> {
    int pointed;
};

template <int N>
int count(Arr<N> items) {
    return items.§(non_type);
}

template <template <class> class C>
int hold(C<int> held) {
    return held.§(template_template);
}

template <class T>
int unwrap(Wrap<T> wrapped) {
    return wrapped.§(explicit_specialization);
}

template <class T>
int deref(Ptr<T> held) {
    return held.§(partial_specialization);
}

template <class T>
int scoped() {
    return Wrap<T>::§(qualified_scope);
}

int use() {
    count(Arr<3>{});
    hold(Box<int>{});
    deref(Ptr<int*>{});
    scoped<int>();
    return unwrap(Wrap<int>{});
}
