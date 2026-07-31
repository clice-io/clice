// Detail rendering for templated symbols: the "template " prefix, class /
// function / variable templates, specializations, deduction guides and
// concepts. Type aliases are deliberately present: they currently do not
// appear in the outline at all.

namespace templates {

template <typename T>
struct Box {
    T value;

    void reset();
};

template <typename T>
void Box<T>::reset() {}

template <>
struct Box<void> {};

template <typename T>
struct Box<T*> {
    T* pointee;
};

template <typename T>
T zero() {
    return T();
}

template <>
int zero<int>();

template <typename T>
constexpr T pi = T(3.14159);

template <typename T>
concept Small = sizeof(T) <= 4;

struct Guide {
    template <typename T>
    Guide(T raw);
};

template <typename T>
Guide(T*) -> Guide;

using IntBox = Box<int>;

template <typename T>
using BoxOf = Box<T>;

}  // namespace templates
