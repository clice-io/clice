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

template <typename T>
T build(int size) {
    return §(parameter)T(size);
}

Holder<int> holder(1);
Widget made = make(2);
Widget built = build<Widget>(3);
