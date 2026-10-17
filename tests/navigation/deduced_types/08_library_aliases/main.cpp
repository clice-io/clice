namespace std {

template <class T>
struct unique_ptr {};

namespace __detail {
template <class T>
using __unique_ptr_t = unique_ptr<T>;
}

template <class T>
__detail::__unique_ptr_t<T> make_unique() {
    return {};
}

template <class T>
struct vector {
    using iterator = T*;
    iterator begin();
};

}  // namespace std

struct Widget {};

void use(std::vector<Widget>& widgets) {
    au§(internal)to made = std::make_unique<Widget>();
    au§(public)to first = widgets.begin();
}
