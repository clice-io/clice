namespace std {

inline namespace __1 {

template <typename Char>
struct basic_string {};

typedef basic_string<char> string;

}  // namespace __1

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

template <typename... Ts>
struct tuple {
    tuple(Ts... values);
};

template <typename State>
struct fpos {};

typedef fpos<int> streampos;
typedef fpos<int> wstreampos;

}  // namespace std

struct Outer {
    template <typename... Ts>
    struct Inner {};
};

void use(std::vector<std::string>& names,
         std::vector<std::fpos<int>>& positions,
         std::vector<Outer::Inner<std::string>>& inners) {
    auto& first = names.front();
    auto shared = std::share_count();
    auto held = std::hold_count();
    auto& position = positions.front();
    std::tuple both(names.front(), 1);
    auto inner = inners.front();
}
