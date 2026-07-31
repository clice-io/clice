// Parameter name hints and their suppression rules: spelled-name match,
// /*name=*/ comments, setters, std builtins, mutable-reference markers,
// pack forwarding, names recovered from definitions, underscore stripping
// and constructors.

namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
struct remove_reference<T&> {
    using type = T;
};

template <typename T>
struct remove_reference<T&&> {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& t) noexcept;

template <typename T>
constexpr typename remove_reference<T>::type&& move(T&& t) noexcept;

}  // namespace std

namespace basic {

void draw(int width, int height);

void use() {
    draw(10, 20);
}

}  // namespace basic

namespace spelled_name_suppression {

void draw(int width, int height);

void use() {
    int width = 5;
    int h = 2;
    draw(width, h);
}

}  // namespace spelled_name_suppression

namespace comment_suppression {

void draw(int width, int height);

void use() {
    draw(/*width=*/1, /*height=*/2);
}

}  // namespace comment_suppression

namespace setters {

struct Config {
    void setWidth(int width);
    void set_timeout(int timeout_millis);
};

void use(Config& config) {
    config.setWidth(3);
    config.set_timeout(5);
}

}  // namespace setters

namespace builtins {

void consume(int&& sink);

void use() {
    int value = 1;
    consume(std::move(value));
}

}  // namespace builtins

namespace ref_markers {

void mutate(int& value);
void observe(const int& value);

void use() {
    int v = 0;
    mutate(v);
    observe(v);
}

}  // namespace ref_markers

namespace forwarding {

void target(int first, int second);

template <typename... Args>
void wrap(Args&&... args) {
    target(std::forward<Args>(args)...);
}

void use() {
    wrap(1, 2);
}

}  // namespace forwarding

namespace names_from_definition {

void resize(int, int);

void use() {
    resize(800, 600);
}

void resize(int width, int height) {}

}  // namespace names_from_definition

namespace underscores {

void fill(int _value, int __count);

void use() {
    fill(1, 2);
}

}  // namespace underscores

// Pack parameters resolve through a plain pass-through call (no
// std::forward needed): the wrapper's pack is mapped to the target's
// named parameters.
namespace pack_forwarding {

void sink(int a, int b, int c);

template <typename... Ts>
void call_with(Ts... ts) {
    sink(ts...);
}

void use() {
    call_with(1, 2, 3);
}

}  // namespace pack_forwarding

namespace constructors {

struct Point {
    Point(int x, int y);
    Point(const Point& other);
};

void use() {
    Point p(1, 2);
    Point q(p);
    Point r{3, 4};
}

}  // namespace constructors
