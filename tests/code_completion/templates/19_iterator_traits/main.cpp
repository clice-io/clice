// An iterator that names what `->` returns through iterator_traits, as the
// standard library's vector iterator does, with the specialization for
// pointers constrained on a type trait.
namespace std {

template <class T>
inline constexpr bool is_object_v = __is_object(T);

template <class It>
struct iterator_traits {
    using pointer = typename It::pointer;
};

template <class T>
    requires is_object_v<T>
struct iterator_traits<T*> {
    using pointer = T*;
};

template <class Ptr>
struct normal_iterator {
    using pointer = typename iterator_traits<Ptr>::pointer;
    pointer operator->() const;
};

template <class T>
struct vector {
    using iterator = normal_iterator<T*>;
    iterator begin();
};

}  // namespace std

template <class T>
struct Wrap {
    T inner;
    int count;
};

template <class T>
void walk(std::vector<Wrap<T>> items) {
    items.begin()->§(call);
    typename std::vector<Wrap<T>>::iterator it;
    it->§(named_type);
}
