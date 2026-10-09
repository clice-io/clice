/// # Definition-scope return type
///
/// - status: supported
///
/// A return type naming a nested type or another namespace is spelled so it resolves at the definition
///
/// Parameter types are looked up in the class scope like the declaration's
/// and stay as written.

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
