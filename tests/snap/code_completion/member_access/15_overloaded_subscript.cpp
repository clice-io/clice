/// # Overloads sharing a return type
///
/// - status: supported
/// - diagnostics: expected
///
/// A dependent call whose candidate overloads all return the same type completes the members of that type
///
/// `table[key].` on a map whose `operator[]` takes either a `const K&` or a
/// `K&&` lists the members of the mapped type. The overloads may be defined
/// outside the class, and the class template redeclared after its
/// definition, as the standard maps are.

// The member accesses dangle; the statements stay semicolon-terminated so a
// later marker is not dragged into recovery.
template <typename T>
struct Vec {
    void push_back(const T& value);
    int size() const;
};

template <typename K, typename V>
struct Pair {
    K first;
    V second;
};

template <typename P>
struct Iterator {
    P* operator->() const;
};

template <typename K, typename V>
struct Map {
    using key_type = K;
    using mapped_type = V;
    using value_type = Pair<const K, V>;
    using iterator = Iterator<value_type>;

    mapped_type& operator[](const key_type& key);
    mapped_type& operator[](key_type&& key);
    iterator begin();
};

template <typename K, typename V>
struct Map;

template <typename K, typename V>
V& Map<K, V>::operator[](const K& key) {
    return begin()->second;
}

template <typename K, typename V>
V& Map<K, V>::operator[](K&& key) {
    return begin()->second;
}

template <typename K, typename T>
void bar(Map<K, Vec<T>> table, K key) {
    table[key].§(subscript);
    table.begin()->second.§(iterator);
}
