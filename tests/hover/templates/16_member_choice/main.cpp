template <class T>
struct VA {
    using type = T;
};

template <class T>
struct VB : virtual VA<T> {
    using type = T*;
};

template <class T>
struct VC : virtual VA<T> {};

template <class T>
struct VD : VC<T>, VB<T> {
    typename VD::§(dominance)type value;
};

struct Small {
    int small;
};

struct Large {
    int large;
};

template <class T>
struct Rows {
    template <class U>
    Small pick();
    Large pick();
};

template <class T>
int pick_rows(Rows<T> rows) {
    return rows.template pick<int>().§(explicit_arguments)small;
}

struct Getter {
    Small get(int);
    Large get(long) const;
};

template <class T>
int const_object(const T& getter) {
    return getter.get(1).§(const_object)large;
}

void use_getter() {
    const_object(Getter());
}

template <class T>
struct Decay {
    template <class U = void>
    static Small take(int (&a)[3]);
    static Large take(int* p);
};

template <class T>
int array_decay(int (&a)[3]) {
    return Decay<T>::take(a).§(array_decay)large;
}

struct Shared {
    int shared;
};

template <class T>
struct Named {
    using type = Shared;
};

template <class T>
struct Both : Named<T>, Named<T*> {
    typename Both::§(same_alias)type value;
};
