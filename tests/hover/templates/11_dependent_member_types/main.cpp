namespace lib {
template <class T>
struct decay {
    typedef T type;
};

template <class T>
using decay_t = typename decay<T>::type;

template <class _Key, class _Tp>
struct map {
    typedef _Tp mapped_type;
};

template <class _Key, class _Tp>
struct map;

template <class _Tp>
struct vector {
    unsigned long size();
};
}  // namespace lib

template <class A>
struct Box {
    const A field;
    lib::decay_t<A> get();
};

template <class T>
struct Wrapper {
    T value;
    auto size() -> decltype(value.size());
};

template <class T>
void use(Box<T&>& box, Box<T>& plain, Wrapper<lib::vector<T>>& wrapper) {
    box.§(field)field;
    plain.§(get)get();
    wrapper.§(size)size();
    typename lib::map<int, T>::§(mapped_type)mapped_type value;
}
