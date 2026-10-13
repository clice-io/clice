// Type names link inside every form a printed type takes: an alias a class
// prefers to be named by, a member pointer, a template template argument,
// and a designator through an array index.
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

struct Corner {
    int x;
    int y;
};

void use() {
    auto label = make_text();
    auto member = sides_member();
    auto uses = make_uses();
    Corner corners[2] = {1, 2, 3, 4};
}
