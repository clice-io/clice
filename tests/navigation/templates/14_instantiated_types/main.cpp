struct Narrow {
    using unit = char;
};

struct Wide {
    using unit = char8_t;
};

template <typename T>
struct Text;

template <>
struct Text<char> {
    using view = Narrow;
};

template <>
struct Text<char8_t> {
    using view = Wide;
};

template <typename T>
struct Source {
    using Unit = typename Text<T>::§(member_alias)view::§(nested)unit;
    typename Text<T>::§(field)view buffer;
    void put(typename Text<T>::§(parameter)view value);
};

template <typename T>
int measure(T) {
    typename Text<T>::§(local)view chosen{};
    return sizeof(chosen);
}

template <typename T>
struct Unused {
    typename Text<T>::§(uninstantiated)view never;
};

template struct Source<char>;
template struct Source<char8_t>;
int measured = measure(u8'a');
