namespace __gnu_cxx {
template <class Pointer, class Container>
struct __normal_iterator {};

template <class Alloc>
struct __alloc_traits {
    typedef typename Alloc::value_type value_type;
};
}  // namespace __gnu_cxx

namespace std {
template <class T>
struct allocator {
    typedef T value_type;
};

template <class T, class Alloc = allocator<T>>
struct vector {
    typedef typename __gnu_cxx::__alloc_traits<Alloc>::value_type& reference;
    typedef __gnu_cxx::__normal_iterator<T*, vector> iterator;
    reference front();
    iterator begin();
};

template <class Container, bool Const>
struct __bit_iterator {};

template <class Alloc>
struct vector<bool, Alloc> {
    typedef __bit_iterator<vector, false> pointer;
    typedef pointer iterator;
    iterator begin();
};

template <class Pointer>
struct __wrap_iter {};

template <class Iterator>
struct reverse_iterator {
    typedef Iterator iterator_type;
    iterator_type base();
};

template <class T>
struct span {
    typedef __wrap_iter<T*> iterator;
    iterator begin();
    iterator end();
    reverse_iterator<iterator> rbegin();
};

template <class Iterator>
Iterator next(Iterator it);

template <class Iterator>
Iterator find(Iterator first, Iterator last);

struct _Bind_front {};
_Bind_front bind_front();
}  // namespace std

void use(std::vector<int>& values, std::span<int> items, std::vector<bool>& flags) {
    auto& first = values.front();
    auto after = std::next(values.begin());
    std::vector<int>::iterator start = values.begin();
    auto second = std::next(start);
    auto flag = flags.begin();
    auto set = std::find(flags.begin(), flags.begin());
    auto backwards = items.rbegin();
    auto forwards = std::next(backwards.base());
    auto found = std::find(items.begin(), items.end());
    auto bound = std::bind_front();
}
