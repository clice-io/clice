// A member call whose overloads include a member template, as std::map's
// find() and its heterogeneous lookup: without explicit template arguments
// the non-templates decide the type.
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
    iterator find(const K& key);
    template <class Other>
    iterator find(const Other& key);
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
