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

template <long Num, long Den = 1>
struct ratio {
    static constexpr long num = Num;
    static constexpr long den = Den;
    typedef ratio<num, den> type;
};

namespace chrono {
template <class Rep, class Period = ratio<1>>
struct duration {};

typedef duration<long> seconds;
}  // namespace chrono

template <class Rep, class Period>
struct common_type<chrono::duration<Rep, Period>, chrono::duration<Rep, Period>> {
    typedef chrono::duration<Rep, typename Period::type> type;
};

namespace chrono {
template <class Rep, class Period>
typename common_type<duration<Rep, Period>, duration<Rep, Period>>::type
    operator-(duration<Rep, Period>, duration<Rep, Period>);
}  // namespace chrono

template <class T>
enable_if_t<true, T> identity(T value);

template <unsigned Index, class... Ts>
__type_pack_element<Index, Ts...> pick(Ts... values);
}  // namespace std

void use(std::chrono::seconds earlier,
         std::chrono::seconds later,
         std::chrono::duration<double, std::ratio<1, 1000>> start,
         std::chrono::duration<double, std::ratio<1, 1000>> stop) {
    auto elapsed = later - earlier;
    auto measured = stop - start;
    auto same = std::identity(1);
    auto first = std::pick<0>(1, 2.0);
}
