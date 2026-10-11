template <typename T>
struct Vec {
    T& operator[](int index);
    const T& operator[](int index) const;

    void push_back(const T& value);
    int size() const;
};

template <typename T>
struct Grid {
    Vec<T> cells;
    Vec<T> spare[2];
};

template <typename T>
void bar(Vec<Vec<T>> rows,
         const Vec<Vec<T>>& fixed,
         Vec<Vec<T>*> pointers,
         const Grid<T>& grid) {
    auto& row = rows[0];
    row.§(reference);
    const auto& view = rows[0];
    view.§(const_reference);
    auto copy = fixed[0];
    copy.§(value);
    auto&& forwarded = fixed[0];
    forwarded.§(forwarding);
    decltype(auto) exact = fixed[0];
    exact.§(decltype_auto);
    auto* pointer = pointers[0];
    pointer->§(pointer);
    decltype(auto) declared = grid.cells;
    declared.§(decltype_member);
    decltype(auto) named = (grid.cells);
    named.§(decltype_parenthesized);
    auto decayed = grid.spare;
    decayed->§(decayed_array);
}
