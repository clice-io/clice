namespace __gnu_cxx {
template <typename Iterator, typename Container>
struct __normal_iterator {};
}  // namespace __gnu_cxx

namespace std {

template <typename T>
struct allocator {};

template <typename T, typename Alloc = allocator<T>>
class vector {
public:
    typedef T* pointer;
    typedef __gnu_cxx::__normal_iterator<pointer, vector> iterator;
    iterator begin();
};

}  // namespace std

namespace app {
void use(std::vector<int>& values) {
    §(keyword)auto §(variable)it = values.begin();
}
}
