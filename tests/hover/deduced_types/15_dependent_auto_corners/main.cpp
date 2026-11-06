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

namespace lib {
struct Item {
    int item;
};

struct Other {
    int other;
};

struct Iter {
    Item& operator*();
    Iter& operator++();
    bool operator!=(const Iter& other) const;
};

Other& operator*(Other& other);

template <class T>
struct Vec {
    T value;
};
}  // namespace lib

template <class T>
struct Rows {
    lib::Iter begin();
    lib::Iter end();
};

template <class T>
T& get(lib::Vec<T>& rows, unsigned long index);

template <class T>
void unrelated_operator(Rows<T> rows, lib::Vec<T> values) {
    for(auto& §(unrelated_operator)row: rows) {}
    auto& §(converted_argument)value = ::get(values, 0);
}

template <class T>
struct Id {
    static lib::Item id(lib::Item item);
};

template <class T>
void nested_calls(lib::Item value) {
    auto §(nested_calls)nested = Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(Id<T>::id(value))))))))))))))))))));
}

struct Picked {
    int picked;
};

Picked pick(lib::Item& item);
lib::Other pick(lib::Item&& item);

template <class T>
struct Forward {
    void f(T& v) {
        auto §(collapsed_cast)x = ::pick(static_cast<T&&>(v));
    }
};

template struct Forward<lib::Item&>;

namespace outer {
inline namespace v1 {
inline namespace v2 {
struct Versioned {};
}  // namespace v2
}  // namespace v1

Picked choose(Versioned versioned);
}  // namespace outer

lib::Other choose(...);

template <class T>
struct Versions {
    using type = outer::Versioned;
};

template <class T>
void nested_inline(typename Versions<T>::type versioned) {
    auto §(nested_inline)chosen = choose(versioned);
}

template <class... Ts>
struct Tuple {};

template <class T>
struct Packs {
    template <class... Ts, class U>
    static Tuple<Ts...> trailing(Ts... ts, U u);
};

template <class T>
void leading_pack() {
    auto §(leading_pack)packed = Packs<T>::trailing(1);
}
