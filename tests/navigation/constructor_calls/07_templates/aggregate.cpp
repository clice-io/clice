template <typename T>
struct Pair {
    Pair() = default;
    T first;
    T second;
};

template <typename T>
Pair<T> empty() {
    return §(aggregate)Pair<T>{};
}

template <typename T>
Pair<T> zeroed() {
    return §(value_initialized)Pair<T>();
}
