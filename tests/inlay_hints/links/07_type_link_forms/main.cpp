// Type names link inside every form a printed type takes: an alias a class
// prefers to be named by, a member pointer, a template template argument,
// deduced class template arguments, and a designator through an array
// index. Once a long type is cut short, a name whose namesake of another
// declaration was cut away stays plain.
template <typename Char>
struct basic_text;

using text = basic_text<char>;

template <typename Char>
struct [[clang::preferred_name(text)]] basic_text {};

text make_text();

struct Shape {
    int sides;
};

int Shape::* sides_member();

template <template <typename> class Holder>
struct Uses {};

template <typename T>
struct Box {};

Uses<Box> make_uses();

namespace a {
template <typename T>
struct Crate {
    Crate(T item);
};
}  // namespace a

namespace b {
struct Crate {};
}  // namespace b

a::Crate crate(b::Crate{});

template <typename T>
struct AnExtraordinarilyLongWrapper {};

template <typename First, typename Second>
struct Duo {};

Duo<AnExtraordinarilyLongWrapper<a::Crate<int>>, b::Crate> make_duo();

struct Corner {
    int x;
    int y;
};

void use() {
    auto label = make_text();
    auto member = sides_member();
    auto uses = make_uses();
    auto duo = make_duo();
    Corner corners[2] = {1, 2, 3, 4};
}
