// A structured binding's type keeps the alias it was declared with, as a
// variable's does: `name` is a `const std::string`.
namespace std {
template <class C>
struct basic_string {};
using string = basic_string<char>;

template <class K, class V>
struct pair {
    K first;
    V second;
};

template <class K, class V>
struct map {
    using value_type = pair<const K, V>;
    value_type* begin();
    value_type* end();
};
}  // namespace std

struct Point {
    int x;
};

void walk(std::map<std::string, Point>& points) {
    §(binding)⟦for (auto& [name, point] : points) {⟧
    }
    std::string label;
    §(variable)⟦auto copy = label;⟧
}
