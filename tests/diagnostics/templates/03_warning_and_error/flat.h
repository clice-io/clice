struct Legacy {
    [[deprecated]] static int get();
};

template <typename T>
int use_flat(T t) {
    int v = T::get();
    return v + t.size();
}
