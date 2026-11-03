namespace instantiation_patterns {

template <typename T, typename Alloc = void> struct Store {
    T* items;
};

template <typename T> struct Store<T*, void> {
    T** slots;
};

template <> struct Store<bool> {
    unsigned bits;
};

template <typename T> struct Queue {
    void push(const T& item);

    template <typename U> U convert() const;

    template <int Capacity> struct Ring {
        T items[Capacity];
    };
};

template <typename T, typename... Rest> void log(T first, Rest... rest);

void use() {
    §(primary)Store<int> numbers;
    §(partial)Store<char*> names;
    §(explicit)Store<bool> flags;

    Queue<long> queue;
    queue.§(member_function)push(1);
    queue.§(member_template)convert<double>();
    Queue<long>::§(member_class_template)Ring<8> ring;

    §(empty_pack)log(1);
}

}
