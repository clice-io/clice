#include <map>
#include <string>
#include <vector>

// A query whose answer would name a declaration or class of the host's library
// asks through a pointer or reference to it, which names none.

namespace standard_map {

template <typename K, typename V>
struct test {
    using §(standard_map)input = typename std::map<K, V>::mapped_type*;
};

}  // namespace standard_map

namespace standard_string {

template <typename T>
struct test {
    using §(standard_string)input = typename std::basic_string<T>::value_type*;
};

}  // namespace standard_string

namespace standard {

template <typename T>
struct test {
    using §(standard)input = typename std::vector<T>::reference&;
};

}  // namespace standard

namespace standard_subscript {

template <typename T>
struct test {
    std::vector<std::vector<T>> rows;
    using §(standard_subscript)input = decltype(rows[0]);
};

}  // namespace standard_subscript

namespace standard_const_overload {

template <typename T>
struct test {
    const std::vector<std::vector<T>> rows;
    using §(standard_const_overload)input = decltype(rows.front());
};

}  // namespace standard_const_overload

namespace standard_map_subscript {

template <typename K, typename V>
struct test {
    std::map<K, std::vector<V>> table;
    K key;
    using §(standard_map_subscript)input = decltype(table[key]);
};

}  // namespace standard_map_subscript

namespace standard_map_iterator {

template <typename K, typename V>
struct test {
    std::map<K, std::vector<V>> table;
    using §(standard_map_iterator)input = decltype(table.begin()->second)*;
};

}  // namespace standard_map_iterator
