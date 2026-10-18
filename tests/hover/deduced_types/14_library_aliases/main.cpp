namespace __gnu_cxx {
template <class Pointer, class Container>
struct __normal_iterator {};

template <class Alloc>
struct __alloc_traits {
    typedef typename Alloc::value_type value_type;
};
}  // namespace __gnu_cxx

namespace std {
typedef unsigned long size_t;

template <class T>
struct allocator {
    typedef T value_type;
};

template <class T, class Alloc = allocator<T>>
struct vector {
    typedef typename __gnu_cxx::__alloc_traits<Alloc>::value_type& reference;
    typedef size_t size_type;
    typedef __gnu_cxx::__normal_iterator<T*, vector> iterator;
    reference front();
    size_type size() const;
    iterator begin();
};

template <class T>
struct __tree_iterator {};

template <class First, class Second>
struct pair {
    First first;
    Second second;
};

template <class T>
struct set {
    typedef __tree_iterator<T> iterator;
    pair<iterator, bool> insert(const T& value);
};

template <class F>
struct invoke_result {
    typedef decltype(static_cast<F (*)()>(nullptr)()()) type;
};

template <class F>
using invoke_result_t = typename invoke_result<F>::type;

template <class F>
invoke_result_t<F> invoke(F function);
}  // namespace std

int answer();

void use(std::vector<int>& values, std::set<int>& ids) {
    auto §(count)count = values.size();
    auto& §(first)first = values.front();
    auto §(iterator)it = values.begin();
    auto §(result)result = std::invoke(answer);
    §(keyword)auto copy = std::invoke(answer);
    auto [§(binding)position, added] = ids.insert(1);
}
