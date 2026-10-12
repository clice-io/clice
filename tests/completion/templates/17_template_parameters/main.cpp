// A template the code after the completion point instantiates once: an
// ordinary function template, a pointer parameter, a class template
// specialization of a namespaced type.
namespace geometry {
struct Point {
    int x;
    int y;
};
}  // namespace geometry

template <class T>
struct Box {
    T value;
    T* get();
};

template <class T>
int read(T shape) {
    return shape.§(function_template);
}

int use(geometry::Point* points) {
    auto deref = [](auto* p) { return p->§(pointer); };
    deref(points);
    auto open = [](auto box) { return box.§(specialization); };
    open(Box<geometry::Point>{});
    return read(geometry::Point{});
}
