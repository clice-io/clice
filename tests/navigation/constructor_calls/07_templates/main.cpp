namespace std {
template <typename E>
class initializer_list {
    const E* first;
    decltype(sizeof(0)) count;
};
}  // namespace std

struct Widget {
    Widget(int size);
    Widget(int width, int height);
};

template <typename T>
struct Box {
    Box(T first, T second);
    Box() : §(delegating)Box(T{}, T{}) {}
};

template <typename T>
struct Holder : Box<T> {
    Holder(T value) : §(base)Box<T>(value, value) {}
};

template <typename T>
Widget make(T size) {
    return §(dependent)Widget(size);
}

struct Any {
    template <typename U>
    Any(U value);
};

template <typename T>
Widget braced(T size) {
    return §(braced)Widget{size, size};
}

template <typename T>
Any wrap(T value) {
    return §(templated)Any(value);
}

struct Pack {
    template <typename... U>
    Pack(U... values);
};

template <typename T>
Pack gather(T value) {
    return §(pack)Pack(value, value);
}

struct List {
    List(std::initializer_list<int> values);
};

template <typename T>
List listed(T value) {
    return §(listed)List{value, value, value};
}

struct Derived : Widget {
    using Widget::Widget;
};

template <typename T>
Derived inherit(T size) {
    return §(inherited)Derived(size);
}

template <typename T>
Widget* allocate(T size) {
    return new §(allocated)Widget(size);
}

template <typename... Sizes>
Widget forward(Sizes... sizes) {
    return §(expanded)Widget(sizes...);
}

template <typename T>
struct Wide {
    Wide(T value);
};

template <>
struct Wide<int> {
    Wide(int first, int second = 0);
};

template <typename T>
struct Narrow : Wide<T> {
    Narrow(T value) : §(specialized)Wide<T>(value) {}
};

template <typename T>
T build(int size) {
    return §(parameter)T(size);
}

Holder<int> holder(1);
Narrow<int> narrow(1);
Widget made = make(2);
Widget built = build<Widget>(3);
