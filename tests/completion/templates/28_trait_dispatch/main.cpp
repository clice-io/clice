// A container that picks its iterator by a trait on its own member types, as
// MSVC's map does: a static member holding `is_same_v<key_type,
// value_type>` chooses between two iterators through `conditional_t`, and a
// type is never the same as one written in itself.
namespace std {

template <class T, class U>
constexpr bool is_same_v = __is_same(T, U);

template <bool B, class T, class F>
struct conditional {
    using type = T;
};

template <class T, class F>
struct conditional<false, T, F> {
    using type = F;
};

template <bool B, class T, class F>
using conditional_t = typename conditional<B, T, F>::type;

template <class K, class V>
struct pair {
    K first;
    V second;
};

}  // namespace std

template <class K, class V>
struct MapTraits {
    using key_type = K;
    using value_type = std::pair<const K, V>;
};

struct Frozen {
    int frozen;
};

template <class T>
struct ConstIterator {
    Frozen* operator->() const;
};

template <class T>
struct Iterator {
    T* operator->() const;
};

template <class Traits>
struct Tree {
    using key_type = typename Traits::key_type;
    using value_type = typename Traits::value_type;
    static constexpr bool is_set = std::is_same_v<key_type, value_type>;
    using iterator = std::conditional_t<is_set, ConstIterator<value_type>, Iterator<value_type>>;
    iterator begin();
};

template <class K, class V>
struct Map : Tree<MapTraits<K, V>> {};

struct Entry {
    int count;
};

template <class K>
void walk(Map<K, Entry> map) {
    map.begin()->§(iterator);
    map.begin()->second.§(member);
}
