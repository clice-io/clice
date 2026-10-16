struct Old {
    [[deprecated]] static int get();
};

template <typename T>
int use(T t) {
    int v = T::get();
    return v + t.size();
}
