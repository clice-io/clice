template <class T, int N>
struct Array {};

template <>
struct Array<§(explicit)char, 4> {};

template <class T>
struct Array<T*, §(partial)2> {};

template <class T>
constexpr int width = 1;

template <>
constexpr int width<§(variable)bool> = 8;

template <class T>
T zero() {
    return T();
}

template <>
int zero<§(function)int>() {
    return 0;
}
