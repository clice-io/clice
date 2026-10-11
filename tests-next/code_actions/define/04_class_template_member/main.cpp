template <typename T, int N = 4>
struct Box {
    T §(get)get() const;
};

template <typename T>
struct Box<T*, 1> {
    T* §(single)single();
};
