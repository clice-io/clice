struct Old {
    [[deprecated]] static int get();
};

template <typename T>
struct Box {
    decltype(T().size()) value;
};

template <typename T>
int use(T t) {
    int v = T::get();
    Box<T> box;
    return v;
}
