// A member call whose overloads include a member template, as std::map's
// find() and its heterogeneous lookup: a template whose return type only
// deduction would tell leaves the type to the others; one that returns
// another type leaves the call untyped.
template <class K, class V>
struct Pair {
    K first;
    V second;
};

template <class K, class V>
struct Map {
    struct iterator {
        Pair<K, V>* operator->() const;
    };
    struct const_iterator {
        const Pair<K, V>* operator->() const;
    };
    iterator find(const K& key);
    const_iterator find(const K& key) const;
    template <class Other>
    iterator find_transparent(const Other& key);
    template <class Other>
    const_iterator find_transparent(const Other& key) const;
    template <class Other>
    auto find(const Other& key) -> decltype(find_transparent(key));
    template <class Other>
    auto find(const Other& key) const -> decltype(find_transparent(key));
};

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(Map<int, Wrap<T>> table) {
    table.find(1)->second.§(member);
}

struct Left {
    int left;
};

struct Right {
    int right;
};

template <class T>
struct Picker {
    Left pick(int index);
    template <class U>
    Right pick(U value);
};

template <class T>
void choose(Picker<T> picker) {
    picker.pick("x").§(differing_template);
}
