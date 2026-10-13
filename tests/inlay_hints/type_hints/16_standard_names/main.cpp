namespace std {

template <typename Char>
struct basic_string {};

typedef basic_string<char> string;

template <typename T>
struct vector {
    T& front();
};

template <typename T>
using _NonArray = T;

template <typename T>
struct shared_ptr {};

shared_ptr<_NonArray<int>> share_count();

template <typename T>
struct __unique_holder {
    using __type = shared_ptr<T>;
};

template <typename T>
using __holder_t = typename __unique_holder<T>::__type;

__holder_t<long> hold_count();

}  // namespace std

void use(std::vector<std::string>& names) {
    auto& first = names.front();
    auto shared = std::share_count();
    auto held = std::hold_count();
}
