template <class Key, class Value, class Info, class Bucket>
struct MapIterator {};

template <class Key>
struct KeyInfo {};

template <class Derived, class Key, class Value>
struct MapBase {
    using iterator = MapIterator<Key, Value, KeyInfo<Key>, Derived>;
    iterator find(Key key);
};

template <class Key, class Value>
struct Map : MapBase<Map<Key, Value>, Key, Value> {};

void use(Map<int, long>& map) {
    auto it = map.find(1);
}
