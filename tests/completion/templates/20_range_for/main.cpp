// The variable of a range-based `for` over a dependent range: the element of
// an array, or what the range's iterator dereferences to.
template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
struct Bag {
    T* begin();
    T* end();
};

template <class T>
void walk(Bag<Wrap<T>> bag, Wrap<T> (&array)[4]) {
    for (auto& item : bag) {
        item.§(range);
    }
    for (const auto& item : array) {
        item.§(array);
    }
}
