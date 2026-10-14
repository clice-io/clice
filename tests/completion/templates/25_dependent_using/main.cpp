// A member a using-declaration brings in from a dependent base, as libc++'s
// containers do with their bases' members.
template <class T>
struct Storage {
    T& get();
    using value_type = T;
};

template <class T>
struct Box : Storage<T> {
    using Storage<T>::get;
    using typename Storage<T>::value_type;
    value_type peek();
};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(Box<Wrap<T>> box) {
    box.get().§(function);
    box.peek().§(type);
}
