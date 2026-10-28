template <typename T>
struct Allocator {
    using value_type = T;
};

template <typename A>
struct AllocatorTraits {
    using value_type = typename A::value_type;
    using pointer = value_type*;
};

template <typename P>
struct Iterator {
    P operator->() const;
};

template <typename T, typename A = Allocator<T>>
struct Vec {
    using value_type = T;
    using reference = value_type&;
    using const_reference = const value_type&;
    using iterator = Iterator<typename AllocatorTraits<A>::pointer>;

    reference operator[](int index);
    const_reference operator[](int index) const;
    reference front();
    const_reference front() const;
    iterator begin();

    void push_back(const T& value);
    int size() const;
};

template <typename T>
struct Grid {
    Vec<Vec<T>> rows;
    mutable Vec<Vec<T>> cache;
};

template <typename T>
void bar(Vec<Vec<T>> rows, const Vec<Vec<T>>& fixed, const Grid<T>& grid) {
    rows[0].§(subscript);
    rows.front().§(call);
    fixed[0].§(const_object);
    grid.rows.front().§(const_member);
    grid.cache.front().§(mutable_member);
    rows.begin()->§(iterator);
}
