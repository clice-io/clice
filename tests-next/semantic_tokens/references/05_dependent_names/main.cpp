template <typename T>
struct Box {
    using value_type = int;
    static void reset();
    int size() const;
};

template <typename T>
void resolved(Box<T> box) {
    typename Box<T>::§(value_type_use)value_type item;
    Box<T>::§(reset_use)reset();
    box.§(size_use)size();
}

template <typename T>
void unresolved(T value) {
    typename T::§(value_type_use2)value_type item;
    T::§(reset_use2)reset();
    value.§(size_use2)size();
}
