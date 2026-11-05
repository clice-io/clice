template <class T>
struct A {
    void f() {
        auto §(explicit_member)x = T();
    }
};

template <>
void A<int>::f() {}

void touch() {
    A<int> a;
}

template <class T>
struct W {
    T inner;
};

template <class T>
struct Holder {
    static W<T> make();
    static inline auto from_call = make();

    void g() {
        auto §(own_specialization)y = Holder<W<T>>::from_call;
    }
};

template <class T>
struct Box {
    static Box make();
    int value;
    int arr[2];
};

template <class T>
void temporaries() {
    auto&& §(rvalue_member)m = Box<T>::make().value;
    decltype(auto) §(decltype_member)d = Box<T>::make().value;
    decltype(auto) §(parenthesized_member)p = (Box<T>::make().value);
    auto&& §(rvalue_element)e = Box<T>::make().arr[0];
}

template <class T>
struct Node {
    T value;
    Node<W<T>>* children;
    template <class U>
    U echo(U u);

    void walk() {
        auto §(recursive_class)r = children->echo(value);
    }
};

template <class U>
U&& identity(U&& u);

template <class T>
struct Tied {
    static Tied make();
    int value;
    int (&arr)[2];
};

template <class T>
void forwarded() {
    auto&& §(xvalue_argument)x = ::identity(Tied<T>::make().value);
    auto&& §(reference_member)r = Tied<T>::make().arr[0];
}
