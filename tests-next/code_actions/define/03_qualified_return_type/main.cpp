// A return type naming a nested type or another namespace is spelled so it
// resolves at the definition; parameter types, looked up in the class scope
// there too, stay as written.

namespace geometry {
struct Point {};
}

namespace app {
struct Shape {
    struct Iterator {};
    Iterator §(begin)begin();
    const geometry::Point& §(origin)origin(Iterator it) const;
};
}
