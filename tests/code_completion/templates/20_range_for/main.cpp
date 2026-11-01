// The variable of a range-based `for` over a dependent range: the element of
// an array, or what the range's iterator dereferences to, `begin()` called
// on the range as an lvalue even when it is a temporary. A variable of a
// written dependent type has nothing to deduce.
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

struct Left {
    int left;
};

struct Right {
    int right;
};

template <class T>
struct Split {
    Left* begin() &;
    Left* end() &;
    Right* begin() &&;
    Right* end() &&;
};

template <class T>
Split<T> make_split();

struct Plain {
    Left* begin();
    Left* end();
};

template <class T>
void split(Plain& plain) {
    for (auto& item : make_split<T>()) {
        item.§(temporary_range);
    }
    for (T item : plain) {
        item.§(typed_variable);
    }
}
