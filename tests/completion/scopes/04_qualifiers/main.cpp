namespace outer {

namespace other {
int shared_value();
}

namespace inner {
void run() {
    shared_v§(sibling);
}
}  // namespace inner

}  // namespace outer

namespace lib {
inline namespace v1 {
int versioned_value;
}
}  // namespace lib

namespace lib2 {
namespace __detail {
int hidden_value;
}
int _private_value;
}  // namespace lib2

namespace colors {
enum Color { crimson };
struct Palette {
    static int crimson_count;
};
}  // namespace colors

namespace {
namespace tools {
int tool_value;
}
}  // namespace

namespace geometry {
namespace shapes {}
namespace sh = shapes;
}  // namespace geometry

namespace containers {
template <class T>
struct Vector {};
template <>
struct Vector<int> {};
template <class T>
void fill(T value);
template <>
void fill<int>(int value);
}  // namespace containers

extern "C++" {
namespace linked {
int linked_value;
}
}

namespace app {
namespace lib {}

void run() {
    versio§(shadowed_namespace);
}
}  // namespace app

void use() {
    versio§(inline_namespace);
    hidden§(reserved_namespace);
    priv§(reserved_name);
    _priv§(typed_underscore);
    crims§(enumerator);
    tool_v§(anonymous_namespace);
    shap§(nested_namespace);
    linked_v§(linkage);
    Vect§(class_specialization);
    fil§(function_specialization);
}
