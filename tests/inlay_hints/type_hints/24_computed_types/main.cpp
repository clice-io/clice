namespace std {
template <bool Condition, class T = void>
struct enable_if {};

template <class T>
struct enable_if<true, T> {
    typedef T type;
};

template <bool Condition, class T = void>
using enable_if_t = typename enable_if<Condition, T>::type;

template <class... Ts>
struct common_type;

template <class T>
struct common_type<T, T> {
    typedef T type;
};

namespace chrono {
template <class Rep>
struct duration {};

typedef duration<long> seconds;

template <class Rep>
typename common_type<duration<Rep>, duration<Rep>>::type operator-(duration<Rep>, duration<Rep>);
}  // namespace chrono

template <class T>
enable_if_t<true, T> identity(T value);

template <unsigned Index, class... Ts>
__type_pack_element<Index, Ts...> pick(Ts... values);
}  // namespace std

void use(std::chrono::seconds earlier, std::chrono::seconds later) {
    auto elapsed = later - earlier;
    auto same = std::identity(1);
    auto first = std::pick<0>(1, 2.0);
}
